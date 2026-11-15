/*
 * Input.cpp - reading the buttons.
 *
 * The buttons sit between pin and GND with the internal pull-up on, so pressed
 * means LOW. A level is accepted only if it holds still for DEBOUNCE_MS, and
 * from there the three events are born: EV_PRESS on the press, EV_LONG when the
 * key is still down after LONG_PRESS_MS, EV_CLICK on the release but only if
 * EV_LONG has not already fired.
 *
 * The long press fires as soon as the time is up and not on the release: that
 * way it is felt at the right moment instead of with the finger already lifted.
 */

#include "Input.h"

static const uint8_t PINS[BUTTON_COUNT] = {
  KEY_PINS[0], KEY_PINS[1], KEY_PINS[2],  KEY_PINS[3],
  KEY_PINS[4], KEY_PINS[5], KEY_PINS[6],  KEY_PINS[7],
  KEY_PINS[8], KEY_PINS[9], KEY_PINS[10], KEY_PINS[11],
  PIN_PAGE_KEY
};

struct ButtonState {
  bool     stable;      // true = pressed
  bool     reading;     // last level read, not confirmed yet
  bool     longFired;   // EV_LONG already emitted for this press
  bool     holdFired;   // EV_HOLD already emitted for this press
  uint32_t changedAt;   // when "reading" changed
  uint32_t pressedAt;
};

static ButtonState buttons[BUTTON_COUNT];

// Circular queue: there are never more than a handful of events between one
// pass and the next, so eight slots are plenty.
static const uint8_t QUEUE_SIZE = 8;
static KeyEvent queue[QUEUE_SIZE];
static uint8_t  queueHead = 0, queueTail = 0;

static void push(uint8_t id, uint8_t kind) {
  const uint8_t next = (queueTail + 1) % QUEUE_SIZE;
  if (next == queueHead) return;  // queue full: the oldest event is worth more
  queue[queueTail].id   = id;
  queue[queueTail].kind = kind;
  queueTail = next;
}

void inputBegin() {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    pinMode(PINS[i], INPUT_PULLUP);
    buttons[i].stable    = false;
    buttons[i].reading   = false;
    buttons[i].longFired = false;
    buttons[i].holdFired = false;
    buttons[i].changedAt = now;
    buttons[i].pressedAt = now;
  }
  // The pull-up needs time to pull the lines up before the first reading,
  // otherwise they all look pressed.
  delay(5);
  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    const bool down = (digitalRead(PINS[i]) == LOW);
    buttons[i].stable  = down;
    buttons[i].reading = down;
    // A key already pressed at power-up (the page key asking for the profile
    // menu) must not produce events when it is finally released, nor reopen by
    // itself the menu it has just opened.
    buttons[i].longFired = down;
    buttons[i].holdFired = down;
  }
}

void inputScan() {
  const uint32_t now = millis();

  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    ButtonState &b = buttons[i];
    const bool down = (digitalRead(PINS[i]) == LOW);

    if (down != b.reading) {
      b.reading   = down;
      b.changedAt = now;
    }

    if (down != b.stable && (now - b.changedAt) >= DEBOUNCE_MS) {
      b.stable = down;
      if (down) {
        b.pressedAt = now;
        b.longFired = false;
        b.holdFired = false;
        push(i, EV_PRESS);
      } else if (!b.longFired) {
        push(i, EV_CLICK);
      }
    }

    if (b.stable && !b.longFired && (now - b.pressedAt) >= LONG_PRESS_MS) {
      b.longFired = true;
      push(i, EV_LONG);
    }

    if (b.stable && !b.holdFired && (now - b.pressedAt) >= HOLD_PRESS_MS) {
      b.holdFired = true;
      push(i, EV_HOLD);
    }
  }
}

bool inputNext(KeyEvent &event) {
  if (queueHead == queueTail) return false;
  event = queue[queueHead];
  queueHead = (queueHead + 1) % QUEUE_SIZE;
  return true;
}

bool inputIsDown(uint8_t id) {
  if (id >= BUTTON_COUNT) return false;
  return digitalRead(PINS[id]) == LOW;
}

bool inputIsHeld(uint8_t id) {
  if (id >= BUTTON_COUNT) return false;
  return buttons[id].stable;
}

bool inputAnyDown() {
  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    if (buttons[i].stable) return true;
  }
  return false;
}

void inputFlush() {
  queueHead = queueTail = 0;
}
