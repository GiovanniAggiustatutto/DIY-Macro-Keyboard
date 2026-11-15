/*
 * Input.h - the thirteen buttons.
 *
 * Debouncing and long-press detection, with the events put in a queue: the rest
 * of the firmware consumes them whenever it suits and never has to read the
 * pins on its own.
 */

#pragma once
#include "Config.h"

// The page key follows the twelve macro keys, so a single index covers them all
// and the tables stay aligned with the configurator ids.
constexpr uint8_t PAGE_KEY_ID = KEY_COUNT;
constexpr uint8_t BUTTON_COUNT = KEY_COUNT + 1;

enum EventKind : uint8_t {
  EV_PRESS = 0,  // just pressed: the macro keys fire here
  EV_LONG,       // still pressed after LONG_PRESS_MS, once only
  EV_HOLD,       // still pressed after HOLD_PRESS_MS, once only
  EV_CLICK       // released without having become a long press
};

struct KeyEvent {
  uint8_t id;
  uint8_t kind;
};

void inputBegin();

// Reads the pins, updates the debouncing and fills the queue. It has to be
// called on every pass of loop().
void inputScan();

// Pulls the next event off the queue; returns false when the queue is empty.
bool inputNext(KeyEvent &event);

// Instantaneous state of a button, without going through the queue: it is only
// needed at startup, to know whether the page key is being held down.
bool inputIsDown(uint8_t id);

// Debounced state of a button: the one that produced the events, not the raw
// pin level. It tells whether a key is still down after its event has been
// consumed.
bool inputIsHeld(uint8_t id);

// True if any button is down right now: a resting finger is activity, and
// standby must not kick in while a key is being held.
bool inputAnyDown();

// Empties the queue: after a mode change the leftover events make no sense.
void inputFlush();
