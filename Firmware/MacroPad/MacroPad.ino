/*
 * MacroPad - Raspberry Pi Pico (RP2040)
 *
 * A macro keyboard with twelve keys in four columns of three, an SSD1306 OLED
 * display above each column, and a thirteenth "page" key with its own display
 * on the left. The configuration comes from a microSD card, in the JSON format
 * written by the v2.0.html web configurator.
 *
 * ============================================================================
 * WIRING
 * ============================================================================
 * Displays (I2C0 + PCA9548A multiplexer at 0x70, every OLED at 0x3C):
 *   PCA9548A SDA ---- GP4        SD2/SC2 -> page key display
 *   PCA9548A SCL ---- GP5        SD3/SC3 -> first column from the left
 *   PCA9548A VIN ---- 3V3(OUT)   SD4/SC4 -> second column
 *   PCA9548A GND ---- GND        SD5/SC5 -> third column
 *   A0/A1/A2 to GND              SD6/SC6 -> fourth column
 *
 * microSD reader (SPI0):
 *   MISO -> GP16   CS -> GP17   SCK -> GP18   MOSI -> GP19
 *   VCC to 5V (VBUS) on modules with a regulator and level shifter, to 3V3 on
 *   bare ones. The card must be formatted FAT16 or FAT32: exFAT is not
 *   supported.
 *
 * Buttons, between pin and GND with no external resistors (internal pull-ups):
 *   page  GP6
 *   key0  GP7    key1  GP8    key2  GP9     key3  GP10
 *   key4  GP11   key5  GP12   key6  GP13    key7  GP14
 *   key8  GP15   key9  GP20   key10 GP21    key11 GP22
 *   The ids are the ones used by the configurator: id = column * 3 + row, so
 *   0-2 is the leftmost column and 9-11 the rightmost one.
 *
 * ============================================================================
 * REQUIREMENTS
 * ============================================================================
 * Core      : "Raspberry Pi Pico/RP2040" by Earle F. Philhower III
 * Libraries : U8g2 (oliver kraus), ArduinoJson 7.x (Benoit Blanchon).
 *             Adafruit TinyUSB, SD and SPI already ship with the core.
 * IMPORTANT: Tools > USB Stack > "Adafruit TinyUSB".
 *
 * ============================================================================
 * HOW IT BEHAVES
 * ============================================================================
 * Startup
 *   The preference for which profile to open lives in /startup.cfg, a text file
 *   written by the keyboard itself. If it is missing and the card holds a
 *   single .json, that one is opened and the preference is written again; if
 *   there is more than one, the picker menu opens. Holding the page key at
 *   power-up opens the menu anyway. Errors (no card, no profile, broken JSON)
 *   end up on the displays, and from there the page key goes back to the file
 *   menu.
 *
 * Switching profile while the keyboard is running
 *   Holding the page key for ten seconds reopens the file picker without a
 *   power cycle. After half a second the page menu opens as usual: just keep
 *   holding. A long press on the file menu closes it again, leaving the profile
 *   that was already open. The list is the one read at power-up: to see the
 *   files on another card you still have to reboot.
 *
 * Page key, "next page" mode
 *   A press moves to the next visible page. The display lists up to three pages
 *   with the open one inverted; if there are more than three, a bar on the right
 *   tells where in the list you are, and if there is a single page it stays
 *   white on black. Holding the key opens the same menu as the mode below.
 *
 * Page key, "page menu" mode
 *   The display shows only the open page. A press opens the menu: the pages
 *   take the place of the twelve keys and pressing one of them jumps there.
 *   With more than twelve pages the page key cycles through the menu screens
 *   (the "2/3" counter sits under the heading). Holding it exits without
 *   changing page.
 *
 * Subpages
 *   A key that jumps to a hidden page opens it as a subpage: the page key turns
 *   into "BACK", with the arrow, and returns where you came from.
 *
 * Keys
 *   The label inverts for a moment on every press, and stays inverted until the
 *   action is over: on a long text, which has to be typed one character at a
 *   time, that says it is still typing. Where the countdown bar is shown the
 *   flash is skipped, because the bar already confirms the press by filling the
 *   whole row. When a key has a second action delayed by more than two seconds,
 *   during the wait a white bar runs behind the label and shrinks to the left as
 *   the moment approaches: under the bar the letters are black, outside it they
 *   go back to white on black, and the key name stays readable on both sides of
 *   its edge. An inverted label goes plain for the whole wait, so the bar can be
 *   seen there too. The wait blocks nothing: the other keys keep working.
 *
 * Standby
 *   If the profile asks for it, the displays switch off after the given time.
 *   The first key pressed only wakes them up, without running its macro.
 *
 * ============================================================================
 * Builtin LED: steady on = profile open, slow blink = picker menu open, fast
 * blink = an error is shown on the displays or, if the displays are dark too,
 * the multiplexer is not answering. Serial at 115200 baud (see SERIAL_DEBUG in
 * Config.h to turn the serial log off).
 */

#include "Config.h"
#include "Display.h"
#include "Input.h"
#include "Profile.h"
#include "Actions.h"
#include "Ui.h"

#if SERIAL_DEBUG
// Longest wait for the serial monitor at startup: it only keeps the first
// diagnostic lines from being lost. Set it to 0 to start right away.
static const uint32_t SERIAL_WAIT_MS = 1000;
#endif

// True only when a real error ended up on the displays: the profile picker is
// not an error, it is just the keyboard waiting for an answer.
static bool bootError = false;

static void blinkForever(uint16_t halfPeriodMs) {
  for (;;) {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(halfPeriodMs);
    digitalWrite(LED_BUILTIN, LOW);
    delay(halfPeriodMs);
    usbTask();
  }
}

// Picks the profile to open and loads it, or leaves the picker menu or the
// error message on screen.
static void bootProfile(bool pageKeyHeld) {
  if (!sdBegin()) {
    uiShowError("SD ERROR",
                "No SD card. Please make sure that the card is formatted "
                "FAT16 or FAT32.");
    bootError = true;
    return;
  }

  const uint8_t files = profileScanFiles();
  if (files == 0) {
    uiShowError("NO PROFILE",
                "No .json profile in the root of the card. Export one from the "
                "configurator and copy it there.");
    bootError = true;
    return;
  }

  char preferred[MAX_FILENAME];
  const bool readOk  = profileReadPreference(preferred, sizeof(preferred));
  const bool onCard  = readOk && profileFileIndex(preferred) >= 0;

  // The reason behind the choice goes to the serial port: without it, a menu
  // that opens when it should not looks the same as one that opens for a good
  // reason.
  if (!readOk) {
    LOG_PRINTLN(F("Preference: /startup.cfg missing or holds no name."));
  } else if (!onCard) {
    LOG_PRINTF("Preference: \"%s\" is not on the card.\n", preferred);
  } else {
    LOG_PRINTF("Preference: \"%s\".\n", preferred);
  }

  // The menu opens on request, or when there is no valid preference and the
  // card offers more than one choice.
  if (pageKeyHeld || (!onCard && files > 1)) {
    LOG_PRINTF("Profile picker: %u files on the card%s.\n",
               files, pageKeyHeld ? " (page key held down)" : "");
    uiOpenFileMenu();
    return;
  }

  // With a single file the preference is (re)written by uiLoadFile().
  bootError = !uiLoadFile(onCard ? preferred : profileFileName(0));
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  LOG_BEGIN(115200);

  // The button pins have to be read right away: the page key held down at
  // power-up asks for the profile menu, and it has to be seen before the user
  // lets it go.
  inputBegin();
  const bool pageKeyHeld = inputIsDown(PAGE_KEY_ID);

  usbBegin();

#if SERIAL_DEBUG
  const uint32_t serialStart = millis();
  while (!Serial && (millis() - serialStart) < SERIAL_WAIT_MS) {
    usbTask();
    delay(10);
  }
#endif
  LOG_PRINTLN("");
  LOG_PRINTLN(F("MacroPad - starting up"));

  uint8_t missing = 0;
  if (!displaysBegin(missing)) {
    LOG_PRINTLN(F("No answer from the PCA9548A at 0x70: check SDA/SCL, the "
                  "power supply and the A0/A1/A2 jumpers (0x70-0x77)."));
    blinkForever(120);
  }
  if (missing > 0) {
    LOG_PRINTF("Warning: %u displays out of 5 did not answer.\n", missing);
  }

  uiBegin();
  bootProfile(pageKeyHeld);
  uiRender();
}

void loop() {
  usbTask();
  inputScan();

  KeyEvent event;
  while (inputNext(event)) {
    uiHandleEvent(event);
    // A profile picked from the menu clears the error signal.
    if (profile.loaded) bootError = false;
  }

  uiTick();
  uiRender();

  // Steady on = profile open. Fast blink = error on screen. Slow blink = picker
  // menu open and the keyboard is waiting: that holds when it is opened while
  // running too, with a profile already loaded.
  const uint16_t period = bootError ? 200 : 600;
  const bool     steady = profile.loaded && !uiFileMenuOpen();
  digitalWrite(LED_BUILTIN, steady || ((millis() / period) & 1) ? HIGH : LOW);
}
