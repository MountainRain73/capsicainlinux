#pragma once

#define VERSION "99"

//arbitray limits
#define MAX_VCODES 0x121  //biggest defined code in scancodes.h must be smaller than this
#define MAX_MACRO_LENGTH 200  //stop recording at some point if it was forgotten.
#define MAX_NUM_MACROS 21 //max number of stored macros (mapped later to 1..20, and the 'hard' macro 0)

//constants
#define DISABLED_CONFIG_NUMBER  0
#define DISABLED_CONFIG_NAME "Capsicain disabled. \r\nNo processing, forward everything. \r\nOnly ESC-X and ESC-0..9 work."
#define AHK_HOTKEY1 SC_F14
#define AHK_HOTKEY2 SC_F15

//defaults
#define DEFAULT_ACTIVE_CONFIG 1
#define DEFAULT_ACTIVE_CONFIG_NAME "Config not initialized. Forwarding all keys."
#define DEFAULT_DELAY_FOR_KEY_SEQUENCE_MS 5

#define DEFAULT_START_AHK_ON_STARTUP true
#define DEFAULT_DELAY_FOR_AHK_MS 50

//rewiremap columns
#define REWIRE_COLS 4
#define REWIRE_ROWS MAX_VCODES
#define REWIRE_OUT 0
#define REWIRE_TAP 1
#define REWIRE_DOUBLETAP 2
#define REWIRE_TAPHOLD 3

#define INI_TAG_INCLUDE "INCLUDE"
#define INI_TAG_GLOBAL "GLOBAL"
#define INI_TAG_OPTIONS "OPTION"
#define INI_TAG_REWIRE "REWIRE"
#define INI_TAG_COMBOS "COMBO"
#define INI_TAG_ALPHA_FROM "ALPHA_FROM"
#define INI_TAG_ALPHA_TO "ALPHA_TO"
#define INI_TAG_ALPHA_END "ALPHA_END"