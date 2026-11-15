/*
 * Profile.cpp - from the microSD to the structures in RAM.
 *
 * HOW MUCH MEMORY AND WHERE
 * The Pico has 264 KB of RAM and a typical profile takes up a handful of them,
 * so the file is loaded whole at startup and the SD card is never touched
 * again: pressing a key must not wait for a read. What stays in memory,
 * though, is not the JSON, it is a compact translation:
 *
 *   - the command names ("KEY_LEFT_CTRL", "MEDIA_MUTE", "NOTE_ON") become the
 *     real HID/MIDI codes, so on a key press there is no string to compare;
 *   - the only strings that survive (labels, page names, texts to type) end up
 *     one after another in a single arena;
 *   - "key_colors" is ignored: it describes the configurator's RGB LEDs, which
 *     this keyboard does not have.
 *
 * The result is 316 bytes per page plus the strings. Measured on the test
 * profiles: the three-page test.json (9 KB) becomes 1.4 KB of RAM, a forty-page
 * one (175 KB) takes up 15.7 KB. With 264 KB on board there is room for
 * hundreds of pages, so reading from the SD card while running is unnecessary.
 * The real limit is elsewhere: the arena offsets are 16 bits, so the labels and
 * the texts to type cannot exceed 64 KB in total.
 *
 * WHY ONE PAGE AT A TIME
 * The profile all ends up in RAM, but it does not get there all at once.
 * Feeding the whole file to ArduinoJson would mean holding at the same instant
 * both the document (larger than the file) and the final structures: on that
 * 175 KB profile that would be more than 200 KB out of 264. Here the file is
 * first mapped by a scanner that notes where each page begins and ends, then
 * the pages are read one by one: the parser never holds more than one page at a
 * time, and the peak does not depend on how many pages the profile has.
 */

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <SPI.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <Adafruit_TinyUSB.h>
#include "Profile.h"

#if ARDUINOJSON_VERSION_MAJOR < 7
#error "ArduinoJson 7.x is required: update the library from the Library Manager."
#endif

// 12 MHz is a good compromise on a breadboard; with long wires it is worth
// going lower. Initialisation happens at 400 kHz anyway, as the SD spec wants.
static const uint32_t SD_CLOCK_HZ = SD_SCK_MHZ(12);

Profile profile = {};

static char    fileNames[MAX_PROFILE_FILES][MAX_FILENAME];
static uint8_t fileCount = 0;
static uint16_t warnings = 0;

uint16_t profileWarnings()            { return warnings; }
uint8_t  profileFileCount()           { return fileCount; }
const char *profileFileName(uint8_t i) { return i < fileCount ? fileNames[i] : ""; }

// ============================================================
// SD CARD
// ============================================================
bool sdBegin() {
  // The pins have to be assigned before SD.begin(), which opens the SPI bus.
  SPI.setRX(PIN_SD_MISO);
  SPI.setTX(PIN_SD_MOSI);
  SPI.setSCK(PIN_SD_SCK);
  return SD.begin(PIN_SD_CS, SD_CLOCK_HZ);
}

// name() can return either the path or just the name depending on the backend:
// whatever comes after the last slash is kept in any case.
static const char *baseName(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static bool hasJsonExtension(const char *name) {
  const size_t len = strlen(name);
  return len > 5 && strcasecmp(name + len - 5, ".json") == 0;
}

uint8_t profileScanFiles() {
  fileCount = 0;

  File root = SD.open("/");
  if (!root) return 0;

  for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    if (fileCount >= MAX_PROFILE_FILES) { entry.close(); break; }

    const char *name = entry.name() ? baseName(entry.name()) : "";
    const bool  keep = !entry.isDirectory() &&
                       // "._name.json" and the like are sown by macOS: they are
                       // not profiles
                       name[0] != '.' && name[0] != '_' &&
                       hasJsonExtension(name) &&
                       strlen(name) < MAX_FILENAME;
    if (keep) {
      strncpy(fileNames[fileCount], name, MAX_FILENAME - 1);
      fileNames[fileCount][MAX_FILENAME - 1] = '\0';
      fileCount++;
    }
    entry.close();
  }
  root.close();

  // Alphabetical order: the list has to stay the same on every boot, otherwise
  // the position in the menu would change on its own.
  for (uint8_t i = 1; i < fileCount; i++) {
    char pivot[MAX_FILENAME];
    strcpy(pivot, fileNames[i]);
    int8_t j = i - 1;
    while (j >= 0 && strcasecmp(fileNames[j], pivot) > 0) {
      strcpy(fileNames[j + 1], fileNames[j]);
      j--;
    }
    strcpy(fileNames[j + 1], pivot);
  }
  return fileCount;
}

int16_t profileFileIndex(const char *name) {
  for (uint8_t i = 0; i < fileCount; i++) {
    if (strcasecmp(fileNames[i], name) == 0) return i;
  }
  return -1;
}

// ============================================================
// PREFERENCE FILE
// ============================================================
bool profileReadPreference(char *out, size_t len) {
  out[0] = '\0';

  File f = SD.open(PREFERENCE_PATH, FILE_READ);
  if (!f) return false;

  // The keyboard writes just the file name, but whoever edits it by hand can put
  // comment lines starting with '#' above it: what counts is the first line that
  // is neither empty nor a comment.
  char line[96];
  bool found = false;

  while (!found && f.available()) {
    size_t n = 0;
    bool   tooLong = false;

    // The line has to be consumed to the end even when it does not fit in the
    // buffer: stopping halfway would make its tail pass for a new line, and a
    // slightly long comment would become the name of the profile to open.
    for (int c = f.read(); c >= 0 && c != '\n' && c != '\r'; c = f.read()) {
      if (n < sizeof(line) - 1) line[n++] = (char)c;
      else                      tooLong = true;
    }
    line[n] = '\0';
    if (tooLong) continue;

    char *start = line;
    while (*start == ' ' || *start == '\t') start++;
    char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';

    if (*start && *start != '#') {
      strncpy(out, baseName(start), len - 1);
      out[len - 1] = '\0';
      found = true;
    }
  }
  f.close();
  return found;
}

bool profileWritePreference(const char *name) {
  SD.remove(PREFERENCE_PATH);   // FILE_WRITE appends: the old one has to go

  File f = SD.open(PREFERENCE_PATH, FILE_WRITE);
  if (!f) return false;

  // Just the name, on one line: the file is rewritten on every choice, so
  // anything added to it by hand would live until the next one.
  f.println(name);
  f.close();
  return true;
}

// ============================================================
// STRING ARENA
// ============================================================
// The arena starts small and doubles when needed. It can do that because the
// model holds offsets and not pointers: if realloc moves the block there is
// nothing to fix up. Sizing it on the file instead (which would always be big
// enough, since a string in the JSON takes up more bytes than are needed to
// hold it) would cost a peak as large as the whole file only to throw away
// nine tenths of it. At the end of the load it is shrunk to what is really
// needed.
static const uint32_t ARENA_FIRST = 256;

// The offsets in the model are 16 bits: past 64 KB of strings alone they would
// overlap each other without saying so. It is not a plausible profile, but the
// load has to be stopped before it writes wrong labels.
static const uint32_t ARENA_LIMIT = 0xFFFF;
static bool arenaOverflow = false;

static bool arenaReserve(uint32_t extra) {
  if (profile.arenaUsed + extra > ARENA_LIMIT) {
    arenaOverflow = true;
    return false;
  }
  if (profile.arenaUsed + extra <= profile.arenaSize) return true;

  uint32_t wanted = profile.arenaSize ? profile.arenaSize : ARENA_FIRST;
  while (wanted < profile.arenaUsed + extra) wanted *= 2;

  char *grown = (char *)realloc(profile.arena, wanted);
  if (!grown) return false;

  profile.arena     = grown;
  profile.arenaSize = wanted;
  return true;
}

static uint16_t arenaPut(const char *s, size_t maxLen = 0) {
  if (s == nullptr || *s == '\0') return 0;

  size_t len = strlen(s);
  if (maxLen && len > maxLen) len = maxLen;

  if (!arenaReserve(len + 1)) {
    warnings++;
    return 0;      // the empty string: the key is left without a label
  }
  const uint16_t offset = (uint16_t)profile.arenaUsed;
  memcpy(profile.arena + offset, s, len);
  profile.arena[offset + len] = '\0';
  profile.arenaUsed += len + 1;
  return offset;
}

// ============================================================
// COMMAND NAMES -> CODES
// ============================================================
struct NameCode  { const char *name; uint8_t  code; };
struct NameUsage { const char *name; uint16_t usage; };

static const NameCode MODIFIERS[] = {
  { "LEFT_CTRL",   KEYBOARD_MODIFIER_LEFTCTRL   },
  { "LEFT_SHIFT",  KEYBOARD_MODIFIER_LEFTSHIFT  },
  { "LEFT_ALT",    KEYBOARD_MODIFIER_LEFTALT    },
  { "LEFT_GUI",    KEYBOARD_MODIFIER_LEFTGUI    },
  { "RIGHT_CTRL",  KEYBOARD_MODIFIER_RIGHTCTRL  },
  { "RIGHT_SHIFT", KEYBOARD_MODIFIER_RIGHTSHIFT },
  { "RIGHT_ALT",   KEYBOARD_MODIFIER_RIGHTALT   },
  { "RIGHT_GUI",   KEYBOARD_MODIFIER_RIGHTGUI   }
};

// Letters, digits and function keys follow the order of the HID table and can be
// worked out with a bit of arithmetic, so only the keys with a name of their own
// are left here.
static const NameCode NAMED_KEYS[] = {
  { "ENTER",       HID_KEY_ENTER         },
  { "ESC",         HID_KEY_ESCAPE        },
  { "BACKSPACE",   HID_KEY_BACKSPACE     },
  { "TAB",         HID_KEY_TAB           },
  { "SPACE",       HID_KEY_SPACE         },
  { "MINUS",       HID_KEY_MINUS         },
  { "EQUAL",       HID_KEY_EQUAL         },
  { "LEFT_BRACE",  HID_KEY_BRACKET_LEFT  },
  { "RIGHT_BRACE", HID_KEY_BRACKET_RIGHT },
  { "BACKSLASH",   HID_KEY_BACKSLASH     },
  { "SEMICOLON",   HID_KEY_SEMICOLON     },
  { "APOSTROPHE",  HID_KEY_APOSTROPHE    },
  { "GRAVE",       HID_KEY_GRAVE         },
  { "COMMA",       HID_KEY_COMMA         },
  { "PERIOD",      HID_KEY_PERIOD        },
  { "SLASH",       HID_KEY_SLASH         },
  { "CAPS_LOCK",   HID_KEY_CAPS_LOCK     },
  { "PRINTSCREEN", HID_KEY_PRINT_SCREEN  },
  { "SCROLL_LOCK", HID_KEY_SCROLL_LOCK   },
  { "PAUSE",       HID_KEY_PAUSE         },
  { "INSERT",      HID_KEY_INSERT        },
  { "HOME",        HID_KEY_HOME          },
  { "PAGE_UP",     HID_KEY_PAGE_UP       },
  { "DELETE",      HID_KEY_DELETE        },
  { "END",         HID_KEY_END           },
  { "PAGE_DOWN",   HID_KEY_PAGE_DOWN     },
  { "RIGHT",       HID_KEY_ARROW_RIGHT   },
  { "LEFT",        HID_KEY_ARROW_LEFT    },
  { "DOWN",        HID_KEY_ARROW_DOWN    },
  { "UP",          HID_KEY_ARROW_UP      }
};

static const NameUsage MEDIA_KEYS[] = {
  { "MEDIA_PLAY_PAUSE",  HID_USAGE_CONSUMER_PLAY_PAUSE       },
  { "MEDIA_VOLUME_UP",   HID_USAGE_CONSUMER_VOLUME_INCREMENT },
  { "MEDIA_VOLUME_DOWN", HID_USAGE_CONSUMER_VOLUME_DECREMENT },
  { "MEDIA_MUTE",        HID_USAGE_CONSUMER_MUTE             },
  { "MEDIA_NEXT",        HID_USAGE_CONSUMER_SCAN_NEXT_TRACK      },
  { "MEDIA_PREVIOUS",    HID_USAGE_CONSUMER_SCAN_PREVIOUS_TRACK  },
  { "MEDIA_STOP",        HID_USAGE_CONSUMER_STOP             }
};

static const NameCode MIDI_COMMANDS[] = {
  { "NOTE_ON",        MC_NOTE_ON        },
  { "NOTE_OFF",       MC_NOTE_OFF       },
  { "CONTROL_CHANGE", MC_CONTROL_CHANGE },
  { "MMC_PLAY",       MC_MMC_PLAY       },
  { "MMC_STOP",       MC_MMC_STOP       },
  { "MMC_RECORD",     MC_MMC_RECORD     }
};

static const NameCode DEVICE_ACTIONS[] = {
  { "STANDBY",    DEV_STANDBY    },
  { "PAGE_MENU",  DEV_PAGE_MENU  },
  { "GO_TO_PAGE", DEV_GO_TO_PAGE }
};

template <size_t N>
static bool lookup(const NameCode (&table)[N], const char *name, uint8_t &out) {
  for (size_t i = 0; i < N; i++) {
    if (strcmp(table[i].name, name) == 0) { out = table[i].code; return true; }
  }
  return false;
}

// A name from the configurator pool -> modifier or HID code.
static bool keycodeFromName(const char *name, uint8_t &code, uint8_t &modifier) {
  code = 0;
  modifier = 0;
  if (name == nullptr || strncmp(name, "KEY_", 4) != 0) return false;
  const char *n = name + 4;

  if (lookup(MODIFIERS, n, modifier)) return true;

  if (n[0] >= 'A' && n[0] <= 'Z' && n[1] == '\0') {
    code = HID_KEY_A + (n[0] - 'A');
    return true;
  }
  if (n[0] >= '0' && n[0] <= '9' && n[1] == '\0') {
    // In the HID table the zero comes after the nine, not before the one.
    code = (n[0] == '0') ? HID_KEY_0 : (uint8_t)(HID_KEY_1 + (n[0] - '1'));
    return true;
  }
  if (n[0] == 'F' && isdigit((unsigned char)n[1])) {
    const int number = atoi(n + 1);
    if (number >= 1 && number <= 12) {
      code = HID_KEY_F1 + (number - 1);
      return true;
    }
  }
  return lookup(NAMED_KEYS, n, code);
}

// ============================================================
// FILE SCANNER
// ============================================================
// Buffered reader: reading one byte at a time from the SD card costs one call
// per byte, and the file is walked twice (once to map the pages, once to read
// them).
struct Scan {
  File    *f;
  uint8_t  buf[64];
  int      len;
  int      pos;
  uint32_t base;   // offset in the file of the first byte of the buffer
};

static void scanSeek(Scan &s, uint32_t offset) {
  s.f->seek(offset);
  s.base = offset;
  s.len  = 0;
  s.pos  = 0;
}

static inline uint32_t scanTell(const Scan &s) { return s.base + s.pos; }

static int scanNext(Scan &s) {
  if (s.pos >= s.len) {
    s.base += s.len;
    s.pos = 0;
    const int n = s.f->read(s.buf, sizeof(s.buf));
    if (n <= 0) { s.len = 0; return -1; }
    s.len = n;
  }
  return s.buf[s.pos++];
}

// Looks for the square bracket that opens the top-level "pages" array and
// leaves the position right after it. Strings and escapes are followed, so a
// label that happened to contain "pages" does not lead it astray.
static bool findPagesArray(Scan &s, uint32_t &firstElement) {
  scanSeek(s, 0);

  int  depth = 0;
  bool inString = false, escaped = false, keyIsPages = false;
  char key[8];
  uint8_t keyLen = 0;
  bool capturing = false;

  for (;;) {
    const int c = scanNext(s);
    if (c < 0) return false;

    if (inString) {
      if (escaped)            escaped = false;
      else if (c == '\\')     escaped = true;
      else if (c == '"') {
        inString = false;
        if (capturing) {
          capturing = false;
          if (keyLen < sizeof(key)) {
            key[keyLen] = '\0';
            keyIsPages = (strcmp(key, "pages") == 0);
          } else {
            keyIsPages = false;          // longer than "pages": it is not it
          }
        }
      } else if (capturing) {
        if (keyLen < sizeof(key) - 1) key[keyLen++] = (char)c;
        else keyLen = sizeof(key);       // too long: stop copying
      }
      continue;
    }

    switch (c) {
      case '"':
        inString = true;
        // Only the field names of the top-level object are of interest.
        capturing = (depth == 1);
        keyLen = 0;
        break;
      case '{':
        depth++;
        break;
      case '[':
        if (depth == 1 && keyIsPages) { firstElement = scanTell(s); return true; }
        depth++;
        break;
      case '}':
      case ']':
        depth--;
        if (depth <= 0) return false;    // the root object is over
        break;
      default:
        break;
    }
  }
}

// From the current position: skips spaces and commas, then returns the range of
// the next object of the array and stops right after it. Returns false on the
// bracket that closes the array.
static bool nextObject(Scan &s, uint32_t &start, uint32_t &end) {
  int c;
  do { c = scanNext(s); } while (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',');
  if (c != '{') return false;

  start = scanTell(s) - 1;

  int  depth = 1;
  bool inString = false, escaped = false;
  while (depth > 0) {
    c = scanNext(s);
    if (c < 0) return false;

    if (inString) {
      if (escaped)        escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"')  inString = false;
      continue;
    }
    if (c == '"')                   inString = true;
    else if (c == '{' || c == '[')  depth++;
    else if (c == '}' || c == ']')  depth--;
  }
  end = scanTell(s);
  return true;
}

// ============================================================
// FROM THE JSON TO THE MODEL
// ============================================================
static void clearAction(Action &action) {
  memset(&action, 0, sizeof(action));
  action.type = ACT_NONE;
}

static uint8_t clampMidi(JsonVariantConst value, uint8_t fallback) {
  if (value.isNull()) return fallback;
  const long raw = value.is<const char *>() ? atol(value.as<const char *>())
                                            : value.as<long>();
  if (raw < 0)   return 0;
  if (raw > 127) return 127;
  return (uint8_t)raw;
}

static void buildAction(const char *type, JsonArrayConst commands, Action &out) {
  clearAction(out);
  if (type == nullptr) return;

  const size_t count = commands.isNull() ? 0 : commands.size();
  const char  *first = count > 0 ? commands[0].as<const char *>() : nullptr;

  if (strcmp(type, "SHORTCUT") == 0) {
    out.type = ACT_SHORTCUT;
    for (JsonVariantConst command : commands) {
      const char *name = command.as<const char *>();
      uint8_t code, modifier;
      if (!keycodeFromName(name, code, modifier)) {
        LOG_PRINTF("  ! unknown key code: %s\n", name ? name : "(not a string)");
        warnings++;
        continue;
      }
      if (modifier) {
        out.chord.modifiers |= modifier;
      } else if (out.chord.count < MAX_CHORD) {
        out.chord.keys[out.chord.count++] = code;
      } else {
        LOG_PRINTLN(F("  ! more than six keys together: the others are ignored"));
        warnings++;
      }
    }
    if (out.chord.modifiers == 0 && out.chord.count == 0) out.type = ACT_NONE;

  } else if (strcmp(type, "MULTIMEDIA") == 0) {
    for (size_t i = 0; i < sizeof(MEDIA_KEYS) / sizeof(MEDIA_KEYS[0]); i++) {
      if (first && strcmp(MEDIA_KEYS[i].name, first) == 0) {
        out.type = ACT_MULTIMEDIA;
        out.media.usage = MEDIA_KEYS[i].usage;
        return;
      }
    }
    LOG_PRINTF("  ! unknown multimedia command: %s\n", first ? first : "(empty)");
    warnings++;

  } else if (strcmp(type, "MIDI") == 0) {
    uint8_t command;
    if (first == nullptr || !lookup(MIDI_COMMANDS, first, command)) {
      LOG_PRINTF("  ! unknown MIDI command: %s\n", first ? first : "(empty)");
      warnings++;
      return;
    }
    out.type = ACT_MIDI;
    out.midi.command = command;
    out.midi.v1 = clampMidi(count > 1 ? commands[1] : JsonVariantConst(), 60);
    out.midi.v2 = clampMidi(count > 2 ? commands[2] : JsonVariantConst(), 127);

  } else if (strcmp(type, "TEXT") == 0) {
    if (first == nullptr || *first == '\0') return;
    out.type = ACT_TEXT;
    out.text.offset = arenaPut(first);

  } else if (strcmp(type, "DEVICE") == 0) {
    uint8_t action;
    if (first == nullptr || !lookup(DEVICE_ACTIONS, first, action)) {
      LOG_PRINTF("  ! unknown system action: %s\n", first ? first : "(empty)");
      warnings++;
      return;
    }
    out.type = ACT_DEVICE;
    out.device.action = action;
    // The destination is the page number the configurator writes in the profile,
    // counted from 1, and it is kept here as the index of that page; the range
    // is checked at the end, when the number of pages is known.
    if (action == DEV_GO_TO_PAGE && count > 1) {
      const char *target = commands[1].as<const char *>();
      const int page = target ? atoi(target) : commands[1].as<int>();
      out.device.page = (uint16_t)(page > 0 ? page - 1 : 0);
    }

  } else {
    LOG_PRINTF("  ! unknown action type: %s\n", type);
    warnings++;
  }
}

static void buildKey(JsonObjectConst src, KeyCfg &dst) {
  dst.label         = arenaPut(src["label"] | "", MAX_LABEL);
  dst.inverted      = src["inverted"] | false;
  dst.secondEnabled = src["second_action_enabled"] | false;

  const float delay = src["second_action_delay"] | 0.0f;
  const long  deci  = lroundf(delay * 10.0f);
  dst.secondDelayDs = (uint16_t)constrain(deci, 0L, 6000L);

  buildAction(src["type"] | "SHORTCUT", src["commands"].as<JsonArrayConst>(), dst.first);

  if (dst.secondEnabled) {
    buildAction(src["second_action_type"] | "SHORTCUT",
                src["second_action_commands"].as<JsonArrayConst>(), dst.second);
  } else {
    clearAction(dst.second);
  }
}

static void buildPage(JsonObjectConst src, PageCfg &dst, uint16_t index) {
  const char *name = src["name"] | "";
  if (*name == '\0') {
    char fallback[16];
    snprintf(fallback, sizeof(fallback), "Page %u", index + 1);
    dst.name = arenaPut(fallback, MAX_LABEL);
  } else {
    dst.name = arenaPut(name, MAX_LABEL);
  }
  dst.visible = src["visible"] | true;

  for (uint8_t i = 0; i < KEY_COUNT; i++) {
    memset(&dst.keys[i], 0, sizeof(KeyCfg));
    clearAction(dst.keys[i].first);
    clearAction(dst.keys[i].second);
  }

  JsonArrayConst keys = src["keys"];
  uint8_t position = 0;
  for (JsonObjectConst key : keys) {
    // The slot is the one given by "id", which the configurator counts from 1;
    // if it is missing or out of range, the order in which the keys appear in
    // the file is what counts.
    const int id = key["id"] | 0;
    const uint8_t slot = (id >= 1 && id <= KEY_COUNT) ? (uint8_t)(id - 1) : position;
    if (slot < KEY_COUNT) buildKey(key, dst.keys[slot]);
    position++;
  }
}

// ============================================================
// LOADING
// ============================================================
void profileFree() {
  free(profile.pages);
  free(profile.arena);
  // With no profile loaded the fallbacks of Config.h are what is left: nobody
  // uses them (with no pages there is nothing to run), but they are never
  // absurd values.
  profile.keyboardLayout = DEFAULT_KEYBOARD_LAYOUT;
  profile.midiChannel    = DEFAULT_MIDI_CHANNEL;
  profile.pages     = nullptr;
  profile.arena     = nullptr;
  profile.pageCount = 0;
  profile.arenaSize = 0;
  profile.arenaUsed = 0;
  profile.loaded    = false;
  profile.file[0]   = '\0';
}

static bool fail(File &f, char *err, size_t errLen, const char *message) {
  snprintf(err, errLen, "%s", message);
  if (f) f.close();
  profileFree();
  return false;
}

bool profileLoad(const char *fileName, char *err, size_t errLen) {
  profileFree();
  warnings = 0;
  arenaOverflow = false;
  err[0] = '\0';

  char path[MAX_FILENAME + 2];
  snprintf(path, sizeof(path), "/%s", baseName(fileName));

  File f = SD.open(path, FILE_READ);
  if (!f)          return fail(f, err, errLen, "Cannot open the file on the SD card.");
  if (f.isDirectory()) return fail(f, err, errLen, "That name is a folder, not a profile.");

  const uint32_t fileSize = f.size();
  if (fileSize < 2)  return fail(f, err, errLen, "The profile file is empty.");

  // ---- header: everything else is skipped by the filter ----
  {
    JsonDocument filter;
    filter["profile_name"]    = true;
    filter["keyboard_layout"] = true;
    filter["midi_channel"]    = true;
    filter["page_key_mode"]   = true;
    filter["standby_enabled"] = true;
    filter["standby_time"]    = true;

    JsonDocument head;
    const DeserializationError error =
        deserializeJson(head, f, DeserializationOption::Filter(filter));
    if (error) {
      snprintf(err, errLen, "Invalid JSON: %s", error.c_str());
      f.close();
      return false;
    }

    JsonObjectConst root = head.as<JsonObjectConst>();
    if (root.isNull()) return fail(f, err, errLen, "The file is not a MacroPad profile.");

    snprintf(profile.name, sizeof(profile.name), "%s", root["profile_name"] | "MacroPad");

    // Layout of the computer keyboard: anything other than "IT" counts as US,
    // including the field missing in profiles written before it existed.
    profile.keyboardLayout = strcasecmp(root["keyboard_layout"] | "US", "IT") == 0
                             ? LAYOUT_IT : LAYOUT_US;

    // MIDI channel of the device: the configurator writes 1-16. Out-of-range
    // values can only come from a file edited by hand, and a zero would send the
    // messages on channel 16 (the channel code is the number minus one).
    const int channel = root["midi_channel"] | (int)DEFAULT_MIDI_CHANNEL;
    profile.midiChannel = (channel >= 1 && channel <= 16) ? (uint8_t)channel
                                                          : DEFAULT_MIDI_CHANNEL;
    if (profile.midiChannel != channel) {
      LOG_PRINTF("  ! midi_channel %d out of range, set to %u\n",
                 channel, profile.midiChannel);
      warnings++;
    }

    profile.pageKeyMode = strcmp(root["page_key_mode"] | "NEXT_PAGE", "PAGE_MENU") == 0
                          ? MODE_PAGE_MENU : MODE_NEXT_PAGE;
    profile.standbyEnabled = root["standby_enabled"] | false;

    // The configurator writes seconds (the field is mm:ss in the interface). An
    // out-of-range value can only come from a file edited by hand: zero would
    // switch the displays off instantly and a huge number would overflow the
    // count in milliseconds.
    const uint32_t standby = root["standby_time"] | 150UL;
    profile.standbySeconds = constrain(standby, 5UL, 86400UL);
    if (profile.standbySeconds != standby) {
      LOG_PRINTF("  ! standby_time %lu out of range, set to %lu s\n",
                 (unsigned long)standby, (unsigned long)profile.standbySeconds);
      warnings++;
    }
  }

  // ---- mapping the pages ----
  Scan scan;
  scan.f = &f;

  uint32_t firstElement = 0;
  if (!findPagesArray(scan, firstElement)) {
    return fail(f, err, errLen, "The profile has no \"pages\" list.");
  }

  uint32_t start = 0, end = 0;
  uint16_t pageCount = 0;
  scanSeek(scan, firstElement);
  while (nextObject(scan, start, end)) pageCount++;
  if (pageCount == 0) return fail(f, err, errLen, "The profile has no pages.");

  // ---- memory ----
  profile.arena     = (char *)malloc(ARENA_FIRST);
  profile.pages     = (PageCfg *)calloc(pageCount, sizeof(PageCfg));
  if (!profile.arena || !profile.pages) {
    return fail(f, err, errLen, "Not enough memory for this profile.");
  }
  profile.arenaSize = ARENA_FIRST;
  profile.arena[0]  = '\0';   // offset 0 is always the empty string
  profile.arenaUsed = 1;
  profile.pageCount = pageCount;

  // ---- one page at a time ----
  scanSeek(scan, firstElement);
  for (uint16_t i = 0; i < pageCount; i++) {
    if (!nextObject(scan, start, end)) {
      return fail(f, err, errLen, "The list of pages ends too early.");
    }

    f.seek(start);
    JsonDocument page;
    const DeserializationError error = deserializeJson(page, f);
    if (error) {
      // Pages are numbered from 1 wherever they are shown, as in the profile
      snprintf(err, errLen, "Page %u is malformed: %s", i + 1, error.c_str());
      f.close();
      profileFree();
      return false;
    }
    buildPage(page.as<JsonObjectConst>(), profile.pages[i], i);

    // deserializeJson has moved the file on its own: the scanner starts again
    // from where the page ends, which it had already noted down.
    scanSeek(scan, end);
  }
  f.close();

  // ---- final touches ----
  // A key pointing at a page that does not exist (a profile edited by hand) is
  // brought back inside the range instead of sending the keyboard elsewhere.
  for (uint16_t p = 0; p < pageCount; p++) {
    for (uint8_t k = 0; k < KEY_COUNT; k++) {
      Action *actions[2] = { &profile.pages[p].keys[k].first, &profile.pages[p].keys[k].second };
      for (uint8_t a = 0; a < 2; a++) {
        if (actions[a]->type != ACT_DEVICE) continue;
        if (actions[a]->device.action != DEV_GO_TO_PAGE) continue;
        if (actions[a]->device.page >= pageCount) {
          actions[a]->device.page = pageCount - 1;
          warnings++;
        }
      }
    }
  }

  // At least one page has to stay visible or the keyboard would have nothing to
  // show at power-up.
  bool anyVisible = false;
  for (uint16_t p = 0; p < pageCount && !anyVisible; p++) anyVisible = profile.pages[p].visible;
  if (!anyVisible) {
    profile.pages[0].visible = true;
    warnings++;
  }

  // The last doubling left some free space at the end: give it back to the heap.
  char *shrunk = (char *)realloc(profile.arena, profile.arenaUsed);
  if (shrunk) {
    profile.arena     = shrunk;
    profile.arenaSize = profile.arenaUsed;
  }

  if (arenaOverflow) {
    snprintf(err, errLen, "Too much text in this profile: 64 KB of labels is the limit.");
    profileFree();
    return false;
  }

  snprintf(profile.file, sizeof(profile.file), "%s", baseName(fileName));
  profile.loaded = true;

  LOG_PRINTF("Profile \"%s\": %s keyboard, MIDI channel %u, %u pages, "
             "%lu bytes of strings, %lu bytes of pages, "
             "%lu bytes of free heap.\n",
             profile.name,
             profile.keyboardLayout == LAYOUT_IT ? "IT" : "US",
             profile.midiChannel,
             pageCount, (unsigned long)profile.arenaUsed,
             (unsigned long)(pageCount * sizeof(PageCfg)),
             (unsigned long)rp2040.getFreeHeap());
  return true;
}
