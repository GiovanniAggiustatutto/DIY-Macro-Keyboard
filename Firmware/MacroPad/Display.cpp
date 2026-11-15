/*
 * Display.cpp - drawing on the five OLEDs.
 *
 * The layout rules (font, truncation with dots, bands, separators, rounded
 * corners) are the ones from Multiple_Oled_Display_Test, generalised just as
 * much as the firmware needs: a variable usable width, because the page key
 * display can have the scrollbar on the right.
 *
 * There are five displays but a single U8g2 object: they are identical and the
 * mux keeps one connected at a time, so the same 1 KB frame buffer is reused
 * instead of keeping five of them in RAM. The channel has to be picked before
 * sending, not before drawing: drawing works in RAM and nothing goes over the
 * bus until the buffer is sent.
 */

#include <ctype.h>
#include <string.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "Display.h"

// ProFont 17: monospaced, capitals 11 px tall, 9 px cells, hence 14 characters
// per row over 128 px. The small font is only used by the menu screen counter,
// which has to sit under the prompt without stealing attention from it.
#define LABEL_FONT u8g2_font_profont17_mr
#define SMALL_FONT u8g2_font_profont11_mr

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// A display that did not answer at startup is skipped for good: without this
// check every drawing would pay the I2C timeout.
// Index 0 = page key, 1..4 = columns from left to right.
static bool present[1 + COL_COUNT] = { false, false, false, false, false };

static const uint8_t DOT_GAP     = 1;  // px between one dot and the next
static const uint8_t EDGE_MARGIN = 4;  // side breathing room of truncated rows
static const uint8_t CORNER_R    = 3;  // radius of the corner rounding
static const uint8_t LABEL_BUF   = 24; // longer than what fits in a row

// ============================================================
// MULTIPLEXER
// ============================================================
// The PCA9548A has a single register: every bit enables a channel. Writing
// 1 << channel leaves only that one connected and the previous channel drops
// off by itself.
static bool muxSelect(uint8_t channel) {
  Wire.beginTransmission(MUX_ADDR);
  Wire.write(1 << channel);
  return Wire.endTransmission() == 0;
}

static bool muxSelectNone() {
  Wire.beginTransmission(MUX_ADDR);
  Wire.write(0x00);
  return Wire.endTransmission() == 0;
}

static bool deviceResponds(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

// Internal index (0 = page) -> multiplexer channel.
static inline uint8_t channelOf(uint8_t screen) {
  return screen == 0 ? MUX_CH_PAGE : (MUX_CH_COL0 + screen - 1);
}

// ============================================================
// TEXT
// ============================================================
static uint8_t dotSize() {
  const uint8_t size = u8g2.getMaxCharWidth() / 3;
  return size < 1 ? 1 : size;
}

static uint8_t ellipsisWidth() {
  return dotSize() * 3 + DOT_GAP * 2;
}

// The dots rest on the baseline like the font's own '.' and use the current
// drawing colour, so they work over an inverted row too. U8g2 draws the glyphs
// above the baseline, so the last pixel row of a capital is baseline-1: the
// dots have to stop there as well.
static void drawEllipsis(int16_t x, int16_t baseline) {
  const uint8_t size = dotSize();
  for (uint8_t i = 0; i < 3; i++) {
    u8g2.drawBox(x + i * (size + DOT_GAP), baseline - size, size, size);
  }
}

// How many characters fit in a width, with or without the trailing dots.
static uint8_t charsFor(int16_t width, bool withEllipsis) {
  const uint8_t cell = u8g2.getMaxCharWidth();
  if (!withEllipsis) return width > 0 ? width / cell : 0;

  const int16_t usable = width - ellipsisWidth() - 2 * EDGE_MARGIN;
  return usable > 0 ? usable / cell : 0;
}

// The labels come from the profile written however they happen to be: the
// display always shows them in upper case, so the rows have the same height and
// no letter drops below the baseline. Labels that are too long are truncated and
// closed by the dots; returns true when it truncated.
static bool buildLabel(const char *src, char *dst, size_t size, int16_t width) {
  if (src == nullptr) src = "";

  size_t limit = charsFor(width, false);
  if (limit > size - 1) limit = size - 1;

  const bool truncated = strlen(src) > limit;
  size_t     copied    = truncated ? charsFor(width, true) : limit;
  if (copied > size - 1) copied = size - 1;

  size_t i = 0;
  for (; i < copied && src[i] != '\0'; i++) {
    dst[i] = toupper((unsigned char)src[i]);
  }
  dst[i] = '\0';
  return truncated;
}

// Writes a string centred on a baseline, truncating it if needed.
static void drawCentered(const char *text, int16_t baseline, int16_t width) {
  char label[LABEL_BUF];
  const bool truncated = buildLabel(text, label, sizeof(label), width);

  const int16_t strWidth  = u8g2.getStrWidth(label);
  const int16_t textWidth = strWidth + (truncated ? ellipsisWidth() : 0);
  // A string wider than the screen starts at 0 instead of hanging off the left.
  const int16_t x = textWidth < width ? (width - textWidth) / 2 : 0;

  u8g2.drawStr(x, baseline, label);
  if (truncated) drawEllipsis(x + strWidth, baseline);
}

// ============================================================
// BANDS
// ============================================================
// The bands split the 64 pixels of the height between them. When the division
// does not come out even, the leftover pixel goes to the middle band, so all the
// others stay equal to each other: with three rows that gives 0-20, 21-42,
// 43-63, with two 0-31 and 32-63 (the separator falls exactly at mid-screen),
// with a single one 0-63.
static inline uint8_t bandTop(uint8_t index, uint8_t count) {
  return index * (SCREEN_H / count) + (index > count / 2 ? SCREEN_H % count : 0);
}

static inline uint8_t bandBottom(uint8_t index, uint8_t count) {
  return bandTop(index, count) + SCREEN_H / count - 1 +
         (index == count / 2 ? SCREEN_H % count : 0);
}

// The key columns always have three rows: shortcuts for whoever has no reason to
// think about the number of bands.
static inline uint8_t rowTop(uint8_t index)    { return bandTop(index, ROWS_PER_COL); }
static inline uint8_t rowBottom(uint8_t index) { return bandBottom(index, ROWS_PER_COL); }

// Vertical centring of a capital inside a band: the text only takes up the
// ascent, the descent must not be counted or the row ends up shifted upwards.
// When the leftover space is odd the extra pixel goes above.
static int16_t baselineIn(int16_t top, int16_t height) {
  const int16_t ascent = u8g2.getAscent();
  return top + (height - ascent + 1) / 2 + ascent - 1;
}

// rightInset is the strip to be left free on the right for the scrollbar: the
// inverted block stops before it, otherwise the bar would sit on white instead
// of on the black of the panel.
static void drawBand(uint8_t index, uint8_t count, const char *text, bool inverted,
                     uint8_t rightInset) {
  const uint8_t top    = bandTop(index, count);
  const uint8_t bottom = bandBottom(index, count);
  const uint8_t height = bottom - top + 1;
  const uint8_t width  = SCREEN_W - rightInset;

  if (inverted) {
    u8g2.setDrawColor(1);
    u8g2.drawBox(0, top, width, height);
  }

  u8g2.setDrawColor(inverted ? 0 : 1);
  drawCentered(text, baselineIn(top, height), width);
  u8g2.setDrawColor(1);
}

// The countdown bar: it starts as the whole row and shrinks to the left as the
// moment approaches. It sits behind the text, not in front of it: instead of
// covering, it flips the pixels it finds, so the black background turns white
// and the letters over it turn black, and the text stays readable both inside
// and outside the bar. Flipping means the bar is white as long as the row under
// it is plain: it is Ui.cpp that guarantees this, by sending inverted labels
// plain for the whole wait.
// It has to be laid down after the text and before the separators.
static void drawWaitBar(uint8_t index, uint8_t width) {
  if (width == 0) return;
  const uint8_t top = rowTop(index);
  u8g2.setDrawColor(2);   // XOR, the only colour that looks at what is already there
  u8g2.drawBox(0, top, width, rowBottom(index) - top + 1);
  u8g2.setDrawColor(1);
}

// The line between two rows is white only if both are normal: next to an
// inverted row it has to be black, that is, it has to be taken out of the white
// block just laid down. The countdown bar does not concern it: it is a single
// line across the whole width, and where the bar runs it blends into its white
// without breaking it.
static void drawSeparators(const bool inverted[], uint8_t count, uint8_t rightInset) {
  for (uint8_t i = 0; i + 1 < count; i++) {
    const bool dark = inverted[i] || inverted[i + 1];
    u8g2.setDrawColor(dark ? 0 : 1);
    u8g2.drawHLine(0, bandBottom(i, count), SCREEN_W - rightInset);
  }
  u8g2.setDrawColor(1);
}

// Rounded corners. The panel is rectangular and there is no frame, so the
// rounding is done by switching off the pixels outside the arc: it only shows
// where the row is inverted, because there the corner is white. It has to be
// done last.
static void roundCorners() {
  u8g2.setDrawColor(0);
  for (uint8_t y = 0; y < CORNER_R; y++) {
    for (uint8_t x = 0; x < CORNER_R; x++) {
      const int16_t dx = CORNER_R - x;
      const int16_t dy = CORNER_R - y;
      if (dx * dx + dy * dy <= CORNER_R * CORNER_R) continue;
      u8g2.drawPixel(x, y);
      u8g2.drawPixel(SCREEN_W - 1 - x, y);
      u8g2.drawPixel(x, SCREEN_H - 1 - y);
      u8g2.drawPixel(SCREEN_W - 1 - x, SCREEN_H - 1 - y);
    }
  }
  u8g2.setDrawColor(1);
}

// ============================================================
// SENDING
// ============================================================
static void beginFrame() {
  u8g2.clearBuffer();
  u8g2.setFont(LABEL_FONT);
  u8g2.setFontMode(1);  // transparent background: the inverted box stays intact
}

// Sends the buffer to one screen's display. With band >= 0 only the band of
// that row goes out: the display refreshes in 8 px blocks, so it is rounded to
// the edges of the tiles that contain it.
static void sendFrame(uint8_t screen, int8_t band) {
  if (screen > COL_COUNT || !present[screen]) return;
  muxSelect(channelOf(screen));

  if (band < 0) {
    u8g2.sendBuffer();
    return;
  }
  const uint8_t tileTop = rowTop(band) / 8;
  const uint8_t tileEnd = rowBottom(band) / 8;
  u8g2.updateDisplayArea(0, tileTop, SCREEN_W / 8, tileEnd - tileTop + 1);
}

// ============================================================
// STARTUP
// ============================================================
bool displaysBegin(uint8_t &missing) {
  missing = 0;

  // The pins have to be assigned before Wire.begin(), which U8g2 calls in
  // begin().
  Wire.setSDA(PIN_SDA);
  Wire.setSCL(PIN_SCL);
  Wire.begin();
  Wire.setClock(I2C_CLOCK_HZ);

  // First everything is disconnected, then the mux is looked for: that way if
  // something answers it really is the mux and not a display left hanging on a
  // channel opened by an earlier boot.
  if (!muxSelectNone() || !deviceResponds(MUX_ADDR)) return false;

  u8g2.setI2CAddress(OLED_ADDR << 1);  // U8g2 wants the 8-bit address
  u8g2.setBusClock(I2C_CLOCK_HZ);

  for (uint8_t screen = 0; screen <= COL_COUNT; screen++) {
    muxSelect(channelOf(screen));
    present[screen] = deviceResponds(OLED_ADDR);
    if (!present[screen]) {
      LOG_PRINTF("Channel %u: no display at 0x3C, skipping it.\n", channelOf(screen));
      missing++;
      continue;
    }
    // begin() sends the power-up sequence to the display on the open channel:
    // they are identical, so the same configuration works for all of them.
    u8g2.begin();
    u8g2.setContrast(255);
  }
  return true;
}

void displaysPower(bool on) {
  for (uint8_t screen = 0; screen <= COL_COUNT; screen++) {
    if (!present[screen]) continue;
    muxSelect(channelOf(screen));
    u8g2.setPowerSave(on ? 0 : 1);
  }
}

// ============================================================
// KEY COLUMNS
// ============================================================
void renderColumn(uint8_t col, const ScreenRows &rows, int8_t onlyRow) {
  if (col >= COL_COUNT) return;

  beginFrame();
  for (uint8_t i = 0; i < ROWS_PER_COL; i++) {
    drawBand(i, ROWS_PER_COL, rows.text[i], rows.inverted[i], 0);
    drawWaitBar(i, rows.bar[i]);
  }
  drawSeparators(rows.inverted, ROWS_PER_COL, 0);
  roundCorners();

  sendFrame(col + 1, onlyRow);
}

// ============================================================
// PAGE KEY DISPLAY
// ============================================================
// Scrollbar: the rows narrow by BAR_ZONE pixels and the cursor runs in the strip
// left free on the right, a white rectangle with rounded corners. The strip does
// not have to be drawn, it is the black of the panel: it is enough for the
// inverted block of the rows to stop before it.
static const uint8_t BAR_W    = 5;              // width of the cursor
static const uint8_t BAR_GAP  = 2;              // gap between the names and the strip
static const uint8_t BAR_ZONE = BAR_W + BAR_GAP;
static const uint8_t BAR_PAD  = 3;              // margin above and below, stays clear of the rounding
static const uint8_t BAR_R    = 2;              // corner radius of the cursor
static const uint8_t BAR_MIN  = 8;              // minimum height of the cursor

static void drawScrollbar(uint16_t firstShown, uint16_t total, uint8_t window) {
  const int16_t trackTop = BAR_PAD;
  const int16_t trackH   = SCREEN_H - 2 * BAR_PAD;

  // The cursor is as long as the slice of the list currently in view.
  int16_t thumbH = (int32_t)trackH * window / total;
  if (thumbH < BAR_MIN) thumbH = BAR_MIN;
  if (thumbH > trackH)  thumbH = trackH;

  const int16_t span  = total - window;          // possible steps
  const int16_t range = trackH - thumbH;
  const int16_t y = trackTop + (span > 0 ? (int32_t)range * firstShown / span : 0);

  u8g2.setDrawColor(1);
  u8g2.drawRBox(SCREEN_W - BAR_W, y, BAR_W, thumbH, BAR_R);
}

void renderPageList(const char *const names[], const bool inverted[],
                    uint8_t rowCount, uint16_t firstShown, uint16_t total) {
  if (rowCount < 1) rowCount = 1;
  if (rowCount > ROWS_PER_COL) rowCount = ROWS_PER_COL;

  // The rows split the screen into as many equal parts as there are: two pages
  // make two halves, three make three bands like the key columns. The bar is
  // only needed when the list does not fit as a whole.
  const bool    scrolling = total > rowCount;
  const uint8_t inset     = scrolling ? BAR_ZONE : 0;

  beginFrame();
  for (uint8_t i = 0; i < rowCount; i++) {
    drawBand(i, rowCount, names[i], inverted[i], inset);
  }
  drawSeparators(inverted, rowCount, inset);
  if (scrolling) drawScrollbar(firstShown, total, rowCount);
  roundCorners();

  sendFrame(0, -1);
}

void renderPageSingle(const char *name, bool inverted) {
  beginFrame();
  if (inverted) {
    u8g2.setDrawColor(1);
    u8g2.drawBox(0, 0, SCREEN_W, SCREEN_H);
    u8g2.setDrawColor(0);
  }
  drawCentered(name, baselineIn(0, SCREEN_H), SCREEN_W);
  u8g2.setDrawColor(1);
  roundCorners();

  sendFrame(0, -1);
}

// The menu icon, the same one for picking a page and for picking a profile: it
// is the mouse cursor without the stem. The triangle is isosceles, that is the
// vertical left side and the 45-degree hypotenuse are the same length (15 times
// the square root of 2 is 21, as much as the height down to the tip of the
// foot), and below the horizontal cut what is left is the little foot, the one
// that in the whole cursor sits to the left of the stem.
// In XBM format: one bit per pixel, the first bit on the left, every row rounded
// up to a whole byte, hence two bytes per row at this width.
static const uint8_t ICON_W = 16;
static const uint8_t ICON_H = 22;
static const uint8_t ICON_POINTER[] = {
  0x01, 0x00,   //  #
  0x03, 0x00,   //  ##
  0x07, 0x00,   //  ###
  0x0f, 0x00,   //  ####
  0x1f, 0x00,   //  #####
  0x3f, 0x00,   //  ######
  0x7f, 0x00,   //  #######
  0xff, 0x00,   //  ########
  0xff, 0x01,   //  #########
  0xff, 0x03,   //  ##########
  0xff, 0x07,   //  ###########
  0xff, 0x0f,   //  ############
  0xff, 0x1f,   //  #############
  0xff, 0x3f,   //  ##############
  0xff, 0x7f,   //  ###############
  0xff, 0xff,   //  ################
  0x3f, 0x00,   //  ######   <- the little foot, clear of the horizontal cut
  0x1f, 0x00,   //  #####
  0x0f, 0x00,   //  ####
  0x07, 0x00,   //  ###
  0x03, 0x00,   //  ##
  0x01, 0x00    //  #
};

// The prompt does not fit on a single row in the big font, and in a smaller one
// it would be hard to read: it wraps after "CHOOSE A", so it stays the same
// height as the key labels.
static const char    MENU_HEAD[] = "CHOOSE A";
static const uint8_t MENU_GAP    = 6;   // px between the icon and the two rows
static const uint8_t MENU_LEAD   = 6;   // px of gap between one row and the next
static const uint8_t COUNTER_H   = 14;  // bottom band reserved for the counter

// Menu heading: icon and prompt to pick something, in the same style as the
// arrow and the wording of the "back" key, plus the screen counter at the
// bottom, which only shows up when there is more than one screen.
void renderPageMenu(uint8_t kind, uint8_t index, uint8_t total) {
  const char *word = (kind == MENU_PROFILES) ? "PROFILE" : "PAGE";

  beginFrame();

  // The dots trailing the second row are the ones from label truncation:
  // narrower and closer together than the font's own three dots.
  const int16_t ascent = u8g2.getAscent();
  const int16_t headW  = u8g2.getStrWidth(MENU_HEAD);
  const int16_t wordW  = u8g2.getStrWidth(word);
  const int16_t tailW  = wordW + ellipsisWidth();
  const int16_t textW  = headW > tailW ? headW : tailW;

  // The two rows sit at the centre of what the counter leaves free; without the
  // counter they sit at the centre of the screen, like "BACK".
  const int16_t blockH = 2 * ascent + MENU_LEAD;
  const int16_t area   = SCREEN_H - (total > 1 ? COUNTER_H : 0);
  const int16_t top    = (area - blockH) / 2;   // first written pixel row
  const int16_t first  = top + ascent;          // the baseline sits under the glyph
  const int16_t second = first + ascent + MENU_LEAD;

  int16_t left = (SCREEN_W - (ICON_W + MENU_GAP + textW)) / 2;
  if (left < 0) left = 0;
  const int16_t textX = left + ICON_W + MENU_GAP;

  // The icon sits at the middle of the two-row block, the way the arrow sits at
  // the middle of the capitals next to it.
  u8g2.drawXBM(left, top + (blockH - ICON_H) / 2, ICON_W, ICON_H, ICON_POINTER);

  // The two rows are stacked centred: "PAGE..." is much shorter than "CHOOSE A"
  // and aligned to the left it would look out of place.
  u8g2.drawStr(textX + (textW - headW) / 2, first, MENU_HEAD);
  const int16_t tailX = textX + (textW - tailW) / 2;
  u8g2.drawStr(tailX, second, word);
  drawEllipsis(tailX + wordW, second);

  if (total > 1) {
    char counter[12];
    snprintf(counter, sizeof(counter), "%u/%u", index + 1, total);

    u8g2.setFont(SMALL_FONT);
    const int16_t line = baselineIn(SCREEN_H - COUNTER_H, COUNTER_H);
    u8g2.drawStr((SCREEN_W - u8g2.getStrWidth(counter)) / 2, line, counter);
    u8g2.setFont(LABEL_FONT);
  }
  roundCorners();

  sendFrame(0, -1);
}

// Back arrow drawn by hand: the font has none, and a '<' character does not read
// as an arrow. It points left and is two pixels thick like the stems of the
// font, otherwise next to the wording it would look faded.
static const int16_t ARROW_W = 11;

static void drawBackArrow(int16_t x, int16_t baseline) {
  // The two lines of the shaft straddle the middle of the capitals, so the arrow
  // is centred on the wording next to it.
  const int16_t mid  = baseline - u8g2.getAscent() / 2;
  const int16_t head = 4;

  u8g2.drawBox(x, mid, ARROW_W, 2);
  for (int16_t t = 0; t < 2; t++) {          // two passes: the head is 2 px thick
    u8g2.drawLine(x + t, mid,     x + head + t, mid - head);
    u8g2.drawLine(x + t, mid + 1, x + head + t, mid + 1 + head);
  }
}

void renderPageBack() {
  beginFrame();

  const int16_t baseline = baselineIn(0, SCREEN_H);
  const int16_t gap   = 7;
  const int16_t textW = u8g2.getStrWidth("BACK");
  const int16_t left  = (SCREEN_W - (ARROW_W + gap + textW)) / 2;

  drawBackArrow(left, baseline);
  u8g2.drawStr(left + ARROW_W + gap, baseline, "BACK");
  roundCorners();

  sendFrame(0, -1);
}

// ============================================================
// MESSAGES ACROSS THE WHOLE KEYBOARD
// ============================================================
// The text is wrapped over the twelve rows of the four columns, in reading
// order: first column from top to bottom, then the second one and so on. That
// is 14 characters per row, hence about 160 in all.
// The rows here are not keys but one single sentence, so the separators must
// not be laid down: the lines would break up the sentences instead of dividing
// something.
void renderMessage(const char *title, const char *body) {
  char lines[KEY_COUNT][MAX_LABEL + 1];
  for (uint8_t i = 0; i < KEY_COUNT; i++) lines[i][0] = '\0';

  // The font is what tells how many characters fit in a row, and to know that it
  // has to be set on the object already.
  u8g2.setFont(LABEL_FONT);
  uint8_t width = charsFor(SCREEN_W, false);
  if (width > MAX_LABEL) width = MAX_LABEL;

  uint8_t row = 0;
  const char *p = body ? body : "";

  while (*p && row < KEY_COUNT) {
    while (*p == ' ') p++;
    if (!*p) break;

    // The last space within the usable width is taken, so words do not get
    // broken; a word longer than a row is truncated.
    uint8_t take = 0, lastSpace = 0;
    while (p[take] && take < width) {
      if (p[take] == ' ') lastSpace = take;
      take++;
    }
    if (p[take] && lastSpace > 0) take = lastSpace;

    memcpy(lines[row], p, take);
    lines[row][take] = '\0';
    row++;
    p += take;
  }

  for (uint8_t col = 0; col < COL_COUNT; col++) {
    beginFrame();
    for (uint8_t i = 0; i < ROWS_PER_COL; i++) {
      drawBand(i, ROWS_PER_COL, lines[col * ROWS_PER_COL + i], false, 0);
    }
    roundCorners();
    sendFrame(col + 1, -1);
  }
  renderPageSingle(title, true);
}
