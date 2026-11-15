/*
 * Actions.h - what the keyboard sends to the computer.
 *
 * Three USB interfaces over a single cable, all from TinyUSB: a real HID
 * keyboard, a multimedia control (consumer control) and a MIDI port.
 */

#pragma once
#include "Config.h"

void usbBegin();

// To be called often: on some cores the TinyUSB task does not run by itself.
void usbTask();

// Runs an action that has already been translated. System actions (standby,
// menu, jump to a page) do not go over USB and are handed to Ui.cpp.
void runAction(const Action &action);
