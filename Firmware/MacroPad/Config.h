/*
 * Config.h - wiring, constants and the profile model kept in RAM.
 *
 * No logic here: only the numbers that describe the hardware and the structures
 * the JSON file is translated into at startup. The rest of the firmware works
 * on these structures and never touches the SD card again.
 */

#pragma once
#include <Arduino.h>

// ============================================================
// SERIAL LOG
// ============================================================
// Every message the firmware prints goes through the LOG_ macros below. Set
// SERIAL_DEBUG to 0 and they compile down to nothing: no Serial.begin(), no
// wait for the monitor at startup, no strings left in flash. The keyboard
// behaves exactly the same either way - the serial log is diagnostics only, and
// the displays are what the firmware uses to talk to whoever is holding it.
#define SERIAL_DEBUG 1

#if SERIAL_DEBUG
  #define LOG_BEGIN(baud)  Serial.begin(baud)
  #define LOG_PRINT(...)   Serial.print(__VA_ARGS__)
  #define LOG_PRINTLN(...) Serial.println(__VA_ARGS__)
  #define LOG_PRINTF(...)  Serial.printf(__VA_ARGS__)
#else
  // (void)0 and not an empty macro: this way a LOG_ call still needs its
  // semicolon and still works as the single statement of an if without braces.
  #define LOG_BEGIN(baud)  ((void)0)
  #define LOG_PRINT(...)   ((void)0)
  #define LOG_PRINTLN(...) ((void)0)
  #define LOG_PRINTF(...)  ((void)0)
#endif

// ============================================================
// WIRING
// ============================================================

// ---- I2C: PCA9548A and the five OLEDs --------------------------------------
constexpr uint8_t  PIN_SDA      = 4;       // GP4
constexpr uint8_t  PIN_SCL      = 5;       // GP5
constexpr uint8_t  MUX_ADDR     = 0x70;    // PCA9548A with A0/A1/A2 to ground
constexpr uint8_t  OLED_ADDR    = 0x3C;    // the same for all: the mux sorts it out
constexpr uint32_t I2C_CLOCK_HZ = 400000;  // fast mode

// Multiplexer channels. The page key display sits on channel 2, then one column
// per channel going from left to right.
constexpr uint8_t MUX_CH_PAGE = 2;
constexpr uint8_t MUX_CH_COL0 = 3;   // columns: 3, 4, 5, 6

// ---- SPI: microSD reader ----------------------------------------------------
constexpr uint8_t PIN_SD_MISO = 16;  // GP16
constexpr uint8_t PIN_SD_CS   = 17;  // GP17
constexpr uint8_t PIN_SD_SCK  = 18;  // GP18
constexpr uint8_t PIN_SD_MOSI = 19;  // GP19

// ---- Buttons ----------------------------------------------------------------
// All of them between GPIO and GND, with no external resistors: the internal
// pull-ups are used, so an idle pin reads HIGH and a pressed one reads LOW.
constexpr uint8_t PIN_PAGE_KEY = 6;  // GP6

// The key id is the one used by the configurator: id = column * 3 + row, that
// is keys 0-2 are in the first column on the left, 3-5 in the second one and so
// on. GP16-GP19 are skipped because the SD reader uses them.
constexpr uint8_t KEY_PINS[] = { 7, 8, 9, 10, 11, 12, 13, 14, 15, 20, 21, 22 };

// ============================================================
// GEOMETRY
// ============================================================
constexpr uint8_t KEY_COUNT    = 12;
constexpr uint8_t COL_COUNT    = 4;
constexpr uint8_t ROWS_PER_COL = 3;
constexpr uint8_t SCREEN_W     = 128;
constexpr uint8_t SCREEN_H     = 64;
constexpr uint8_t MAX_LABEL    = 15;  // the same limit as the configurator

static_assert(sizeof(KEY_PINS) == KEY_COUNT, "one pin per key");

// ============================================================
// BEHAVIOUR
// ============================================================
constexpr uint16_t DEBOUNCE_MS      = 25;    // button debounce
constexpr uint16_t LONG_PRESS_MS    = 600;   // long press of the page key
constexpr uint16_t HOLD_PRESS_MS    = 10000; // very long press: the file menu
constexpr uint16_t FLASH_MS         = 120;   // confirmation flash on the label
constexpr uint16_t COUNTDOWN_MIN_MS = 2000;  // past this wait the countdown bar
                                             // goes on the display

// The bar shrinks in steps instead of pixel by pixel, and the wait dictates the
// step: how much of the row the bar covers in COUNTDOWN_BAR_MS. The display is
// therefore refreshed at a constant rate, whether the wait lasts two seconds or
// ten minutes - what changes is the width of the step, which on long waits
// narrows down to a single pixel and goes no lower. At a fixed pixel step the
// I2C bus would spend half its time redrawing the same row on short waits - a
// display band is about ten milliseconds - and the keyboard would answer late;
// at a fixed time step, on the other hand, a long wait would move the bar in
// rare jerks.
constexpr uint16_t COUNTDOWN_BAR_MS = 60;    // about sixteen refreshes per second

// ---- How long a key stays down ---------------------------------------------
// A shortcut is not a blink: the modifiers go down first, the keys stay pressed
// for a moment and then everything is released. This is needed because many
// programs do not read the USB reports one by one but check now and then how
// the keyboard looks, and a combination that goes down and back up between two
// checks is never seen at all. OBS Studio is the typical case: with its window
// in the background, global shortcuts arrive intermittently if the press lasts
// only a few milliseconds. These values imitate a fast human keystroke and cost
// less than a tenth of a second per press; raising them further makes the
// keyboard slow to answer, because the action blocks everything else until it
// is done.
constexpr uint16_t MODIFIER_LEAD_MS = 15;   // Ctrl/Alt/Shift/Gui go down first
constexpr uint16_t KEY_HOLD_MS      = 50;   // how long the combination is held
constexpr uint16_t KEY_RELEASE_MS   = 15;   // keys come up, then the modifiers

// Between two actions of the same key - the second action with a zero wait -
// there has to be a moment of idle keyboard, or whoever polls the state at
// intervals never sees the gap between the release of the first and the press
// of the second and counts them as one. With two identical shortcuts the case
// is guaranteed: nothing changes between one check and the next, so as far as
// the program is concerned nothing happened.
constexpr uint16_t ACTION_GAP_MS    = 40;   // idle keyboard between first and second

// ---- MIDI channel -----------------------------------------------------------
// The profile carries it, in the "midi_channel" field (1-16): it applies to the
// whole device and not to a single key. MMC travels in SysEx, which has no
// channel, so it ignores this. The value below is the fallback for profiles
// written before the field existed and for out-of-range values.
constexpr uint8_t DEFAULT_MIDI_CHANNEL = 1;

// ---- Computer keyboard layout -----------------------------------------------
// USB does not send characters but key positions: it is the operating system
// that turns them into characters according to the layout it has set. The
// TinyUSB table is the US one, so on an Italian keyboard the symbols land on
// the wrong keys: @, which on US is Shift+2, comes out as " because that is
// what Shift+2 is in Italian. The layout of the computer the MacroPad is
// plugged into is declared by the profile, in the "keyboard_layout" field ("US"
// or "IT"); what is here is only the fallback for files that do not carry it.
// It only concerns text actions: shortcuts send positions and are to be taken
// as they are.
enum KeyboardLayout : uint8_t { LAYOUT_US = 0, LAYOUT_IT };
constexpr uint8_t DEFAULT_KEYBOARD_LAYOUT = LAYOUT_US;

// File names on the SD card
constexpr char PREFERENCE_PATH[] = "/startup.cfg";
constexpr uint8_t MAX_PROFILE_FILES = 36;    // three menu screens
constexpr uint8_t MAX_FILENAME      = 40;

// ============================================================
// PROFILE MODEL
// ============================================================
enum ActionType : uint8_t {
  ACT_NONE = 0, ACT_SHORTCUT, ACT_MULTIMEDIA, ACT_MIDI, ACT_TEXT, ACT_DEVICE
};

enum DeviceAction : uint8_t {
  DEV_STANDBY = 0, DEV_PAGE_MENU, DEV_GO_TO_PAGE
};

enum MidiCommand : uint8_t {
  MC_NOTE_ON = 0, MC_NOTE_OFF, MC_CONTROL_CHANGE, MC_MMC_PLAY, MC_MMC_STOP, MC_MMC_RECORD
};

enum PageKeyMode : uint8_t {
  MODE_NEXT_PAGE = 0, MODE_PAGE_MENU
};

// How many keys can be pressed together in a shortcut: it is the limit of the
// keyboard HID report (six keys besides the modifiers).
constexpr uint8_t MAX_CHORD = 6;

// An action already translated: the JSON names ("KEY_LEFT_CTRL", "MEDIA_MUTE",
// "NOTE_ON"...) have been resolved into codes while loading, so when the key is
// pressed there is no string left to compare.
struct Action {
  uint8_t type;
  union {
    struct { uint8_t modifiers; uint8_t count; uint8_t keys[MAX_CHORD]; } chord;
    struct { uint16_t usage; }                                           media;
    struct { uint8_t command; uint8_t v1; uint8_t v2; }                  midi;
    struct { uint16_t offset; }                                          text;   // inside the arena
    struct { uint8_t action; uint16_t page; }                            device;
  };
};

struct KeyCfg {
  uint16_t label;          // offset into the string arena
  bool     inverted;
  bool     secondEnabled;
  uint16_t secondDelayDs;  // tenths of a second: the configurator goes up to 600.0 s
  Action   first;
  Action   second;
};

struct PageCfg {
  uint16_t name;           // offset into the string arena
  bool     visible;
  KeyCfg   keys[KEY_COUNT];
};

// The whole profile lives in RAM: the pages in a single block and all the
// strings one after another in an arena, so there are no hundreds of scattered
// mallocs and the memory is released with two frees.
struct Profile {
  char      name[33];
  uint8_t   keyboardLayout;  // LAYOUT_US / LAYOUT_IT: which keyboard to type on
  uint8_t   midiChannel;     // 1-16, ready to use (it is not the 0-15 index)
  uint8_t   pageKeyMode;
  bool      standbyEnabled;
  uint32_t  standbySeconds;
  uint16_t  pageCount;
  PageCfg  *pages;
  char     *arena;
  uint32_t  arenaSize;
  uint32_t  arenaUsed;
  bool      loaded;
  char      file[MAX_FILENAME];  // name of the file it comes from, without "/"
};

extern Profile profile;

// Offset 0 of the arena is always the empty string, so a missing field needs no
// checks scattered around the code.
inline const char *arenaStr(uint16_t offset) {
  return profile.arena ? profile.arena + offset : "";
}
