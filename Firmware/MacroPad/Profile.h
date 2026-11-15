/*
 * Profile.h - SD card, list of profiles and loading the JSON into RAM.
 */

#pragma once
#include "Config.h"

// Powers up the SPI bus and mounts the card.
bool sdBegin();

// Looks for .json files in the root of the card and sorts them by name.
// Returns how many it found (MAX_PROFILE_FILES at most).
uint8_t profileScanFiles();
uint8_t profileFileCount();
const char *profileFileName(uint8_t index);
int16_t profileFileIndex(const char *name);   // -1 if it is not there

// Preference file: a one-line text written by the Pico, with the name of the
// profile to open on the next start.
bool profileReadPreference(char *out, size_t len);
bool profileWritePreference(const char *name);

// Reads the file and translates it into the structures of Config.h. On error it
// fills "err" with a sentence ready for the displays and returns false.
bool profileLoad(const char *fileName, char *err, size_t errLen);
void profileFree();

// Number of non-blocking anomalies met during the last load (unknown commands,
// pages without a name...): they only go to the serial port.
uint16_t profileWarnings();
