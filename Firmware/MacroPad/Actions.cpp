/*
 * Actions.cpp - keyboard, multimedia, MIDI and text through TinyUSB.
 *
 * The HID descriptor holds two distinct reports on the same interface: the real
 * keyboard (report 1) and the multimedia control (report 2), which is the one
 * the operating system recognises as volume/play keys and that a normal
 * keyboard cannot send. MIDI is an interface of its own: the computer sees a
 * MIDI port appear, named after the device.
 *
 * IMPORTANT: in the Arduino IDE you need Tools > USB Stack > "Adafruit
 * TinyUSB", otherwise the build stops with an #error.
 */

#include <Adafruit_TinyUSB.h>
#include "Actions.h"
#include "Ui.h"

enum { RID_KEYBOARD = 1, RID_CONSUMER = 2 };

static const uint8_t desc_hid_report[] = {
  TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(RID_KEYBOARD)),
  TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(RID_CONSUMER))
};

// (descriptor, length, protocol, polling in ms, OUT endpoint)
static Adafruit_USBD_HID  usb_hid(desc_hid_report, sizeof(desc_hid_report),
                                  HID_ITF_PROTOCOL_KEYBOARD, 2, false);
static Adafruit_USBD_MIDI usb_midi;

// ASCII -> {shift, keycode} table provided by TinyUSB. US layout: accented
// letters cannot be represented and are skipped.
static const uint8_t ascii2keycode[128][2] = { HID_ASCII_TO_KEYCODE };

// ---- layout correction -----------------------------------------------------
// The table above says "which key of a US keyboard" produces a given character,
// and that position is what goes out over USB. If the computer has a different
// layout it applies its own rules to the same position and prints something
// else. Below are only the differences between the layout declared by the
// profile and the US one: letters, digits, space, comma and period sit in the
// same places and do not appear. A keycode of zero means the character cannot
// be written on that layout with any combination, and the character is skipped.
static constexpr uint8_t SHIFT = KEYBOARD_MODIFIER_LEFTSHIFT;
static constexpr uint8_t ALTGR = KEYBOARD_MODIFIER_RIGHTALT;

struct AsciiOverride { char ch; uint8_t modifiers; uint8_t keycode; };

// Italian (ISO 105 keys, as on Windows and Linux; on macOS AltGr is the right
// Option key and the combinations match). The keycode names stay the US
// keyboard ones because they mean the physical position: HID_KEY_SEMICOLON is
// the "o with grave accent" key, APOSTROPHE the "a with grave accent" one,
// GRAVE the key to the left of the 1.
static const AsciiOverride layoutIt[] = {
  { '"',  SHIFT,         HID_KEY_2             },
  { '#',  ALTGR,         HID_KEY_APOSTROPHE    },  // AltGr + a-grave
  { '&',  SHIFT,         HID_KEY_6             },
  { '\'', 0,             HID_KEY_MINUS         },
  { '(',  SHIFT,         HID_KEY_8             },
  { ')',  SHIFT,         HID_KEY_9             },
  { '*',  SHIFT,         HID_KEY_BRACKET_RIGHT },
  { '+',  0,             HID_KEY_BRACKET_RIGHT },
  { '-',  0,             HID_KEY_SLASH         },
  { '/',  SHIFT,         HID_KEY_7             },
  { ':',  SHIFT,         HID_KEY_PERIOD        },
  { ';',  SHIFT,         HID_KEY_COMMA         },
  { '<',  0,             HID_KEY_EUROPE_2      },  // the extra key left of the Z
  { '=',  SHIFT,         HID_KEY_0             },
  { '>',  SHIFT,         HID_KEY_EUROPE_2      },
  { '?',  SHIFT,         HID_KEY_MINUS         },
  { '@',  ALTGR,         HID_KEY_SEMICOLON     },  // AltGr + o-grave
  { '[',  ALTGR,         HID_KEY_BRACKET_LEFT  },  // AltGr + e-grave
  { '\\', 0,             HID_KEY_GRAVE         },
  { ']',  ALTGR,         HID_KEY_BRACKET_RIGHT },
  { '^',  SHIFT,         HID_KEY_EQUAL         },
  { '_',  SHIFT,         HID_KEY_SLASH         },
  { '`',  0,             0                     },  // does not exist on the Italian layout
  { '{',  ALTGR | SHIFT, HID_KEY_BRACKET_LEFT  },
  { '|',  SHIFT,         HID_KEY_GRAVE         },
  { '}',  ALTGR | SHIFT, HID_KEY_BRACKET_RIGHT },
  { '~',  0,             0                     },  // same
};

// Translates an ASCII character into modifiers + key position for the layout the
// profile declared. Returns false if the character cannot be written.
static bool asciiToKey(uint8_t ch, uint8_t &modifiers, uint8_t &keycode) {
  if (ch >= 128) return false;

  if (profile.keyboardLayout == LAYOUT_IT) {
    for (uint8_t i = 0; i < sizeof(layoutIt) / sizeof(layoutIt[0]); i++) {
      if ((uint8_t)layoutIt[i].ch != ch) continue;
      modifiers = layoutIt[i].modifiers;
      keycode   = layoutIt[i].keycode;
      return keycode != 0;
    }
  }

  keycode   = ascii2keycode[ch][1];
  modifiers = ascii2keycode[ch][0] ? SHIFT : 0;
  return keycode != 0;
}

static const uint16_t HID_READY_TIMEOUT_MS = 100;

void usbTask() {
#ifdef TINYUSB_NEED_POLLING_TASK
  TinyUSBDevice.task();
#endif
}

void usbBegin() {
  // Needed on cores without automatic init.
  if (!TinyUSBDevice.isInitialized()) TinyUSBDevice.begin(0);

  TinyUSBDevice.setManufacturerDescriptor("MacroPad");
  TinyUSBDevice.setProductDescriptor("MacroPad");
  usb_hid.setStringDescriptor("MacroPad Keys");
  usb_midi.setStringDescriptor("MacroPad MIDI");

  usb_hid.begin();
  usb_midi.begin();

  // If the device had already been enumerated, the new interfaces only become
  // visible after a re-enumeration.
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }
}

// Waits for the HID endpoint to be free for the next report.
static bool hidWaitReady() {
  const uint32_t start = millis();
  while (!usb_hid.ready()) {
    if ((millis() - start) >= HID_READY_TIMEOUT_MS) return false;
    usbTask();
    delay(1);
  }
  return true;
}

// A suspended bus has to be woken up or the reports go nowhere.
static bool hidAvailable() {
  if (TinyUSBDevice.suspended()) TinyUSBDevice.remoteWakeup();
  return usb_hid.ready();
}

// ---- shortcuts -------------------------------------------------------------
// The combination is typed the way a hand would type it: first the modifiers go
// down, then the keys, which stay down for a moment, and in the end everything
// is released starting from the keys. Some combinations (Win+R on Windows) do
// not work if they all arrive in the same report, and programs that read the
// keyboard at intervals instead of report by report - OBS Studio with its
// window in the background - miss presses that are too short. The timings are
// in Config.h.
static void runShortcut(const Action &action) {
  if (!hidAvailable()) return;

  uint8_t keys[MAX_CHORD] = { 0 };
  for (uint8_t i = 0; i < action.chord.count && i < MAX_CHORD; i++) {
    keys[i] = action.chord.keys[i];
  }

  uint8_t none[MAX_CHORD] = { 0 };

  if (action.chord.modifiers) {
    if (!hidWaitReady()) return;
    usb_hid.keyboardReport(RID_KEYBOARD, action.chord.modifiers, none);
    delay(MODIFIER_LEAD_MS);
  }

  if (!hidWaitReady()) return;
  usb_hid.keyboardReport(RID_KEYBOARD, action.chord.modifiers, keys);
  delay(KEY_HOLD_MS);

  // The keys come up while the modifiers are still down: the other way round, a
  // program looking at that instant would see the letter pressed on its own.
  if (action.chord.modifiers) {
    if (!hidWaitReady()) return;
    usb_hid.keyboardReport(RID_KEYBOARD, action.chord.modifiers, none);
    delay(KEY_RELEASE_MS);
  }

  if (!hidWaitReady()) return;
  usb_hid.keyboardRelease(RID_KEYBOARD);
}

// ---- text ------------------------------------------------------------------
// One press and one release per character: the release is also what makes it
// possible to type the same letter twice in a row. Waiting on the endpoint acts
// as a brake by itself (2 ms of polling), so no extra delays are needed.
static void typeText(const char *text) {
  if (!hidAvailable()) return;

  for (const char *p = text; *p; ++p) {
    uint8_t modifiers = 0, keycode = 0;
    if (!asciiToKey((uint8_t)*p, modifiers, keycode)) continue;

    uint8_t report[MAX_CHORD] = { keycode, 0, 0, 0, 0, 0 };

    // The modifiers go out in a report of their own, as in the shortcuts: with
    // AltGr sent together with the key, some systems have time to read the
    // position at its base level and print the wrong character.
    if (modifiers) {
      uint8_t none[MAX_CHORD] = { 0 };
      if (!hidWaitReady()) return;
      usb_hid.keyboardReport(RID_KEYBOARD, modifiers, none);
    }

    if (!hidWaitReady()) return;
    usb_hid.keyboardReport(RID_KEYBOARD, modifiers, report);

    if (!hidWaitReady()) return;
    usb_hid.keyboardRelease(RID_KEYBOARD);
  }
}

// ---- multimedia ------------------------------------------------------------
// The consumer report is a 16-bit usage: the key code is sent and right after
// it a zero, which is the release.
static void runMedia(const Action &action) {
  if (!hidAvailable()) return;

  if (!hidWaitReady()) return;
  usb_hid.sendReport16(RID_CONSUMER, action.media.usage);
  delay(KEY_HOLD_MS);

  if (!hidWaitReady()) return;
  usb_hid.sendReport16(RID_CONSUMER, 0);
}

// ---- MIDI ------------------------------------------------------------------
// write() goes through the TinyUSB stream, which takes care by itself of
// packing the bytes into the four-byte USB-MIDI packets, SysEx included.
static void midiSend(const uint8_t *bytes, size_t length) {
  usb_midi.write(bytes, length);
  usb_midi.flush();
}

// MMC: universal real-time SysEx message to every device
// (F0 7F <id> 06 <command> F7). The commands are 1 = stop, 2 = play,
// 6 = record.
static void midiMmc(uint8_t command) {
  const uint8_t message[] = { 0xF0, 0x7F, 0x7F, 0x06, command, 0xF7 };
  midiSend(message, sizeof(message));
}

static void runMidi(const Action &action) {
  // The profile keeps the channel as the user writes it (1-16); MIDI messages
  // carry its index, from 0 to 15.
  const uint8_t channel = (profile.midiChannel - 1) & 0x0F;

  switch (action.midi.command) {
    case MC_NOTE_ON: {
      const uint8_t message[] = { (uint8_t)(0x90 | channel), action.midi.v1, action.midi.v2 };
      midiSend(message, sizeof(message));
      break;
    }
    case MC_NOTE_OFF: {
      const uint8_t message[] = { (uint8_t)(0x80 | channel), action.midi.v1, action.midi.v2 };
      midiSend(message, sizeof(message));
      break;
    }
    case MC_CONTROL_CHANGE: {
      const uint8_t message[] = { (uint8_t)(0xB0 | channel), action.midi.v1, action.midi.v2 };
      midiSend(message, sizeof(message));
      break;
    }
    case MC_MMC_PLAY:   midiMmc(0x02); break;
    case MC_MMC_STOP:   midiMmc(0x01); break;
    case MC_MMC_RECORD: midiMmc(0x06); break;
    default: break;
  }
}

// ============================================================
void runAction(const Action &action) {
  switch (action.type) {
    case ACT_SHORTCUT:   runShortcut(action); break;
    case ACT_MULTIMEDIA: runMedia(action);    break;
    case ACT_MIDI:       runMidi(action);     break;
    case ACT_TEXT:       typeText(arenaStr(action.text.offset)); break;
    case ACT_DEVICE:     uiDeviceAction(action.device.action, action.device.page); break;
    default: break;
  }
}
