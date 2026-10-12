#pragma once

#include <string>
#include <vector>

struct VKeyEvent
{
    int vcode = 0;
    bool isDownstroke = true;
};

// Convert PS/2 Set 1 scancode to Linux evdev keycode
int ps2ToLinuxKeycode(int ps2_scancode);

// Convert Linux evdev keycode to PS/2 Set 1 scancode
int linuxToPs2Keycode(int linux_keycode);

// Initialize: open all keyboard evdev devices, create uinput device
// Returns true on success
bool linux_input_init();

// Wait for and receive the next keyboard event (blocking)
// Returns the PS/2 Set 1 scancode and isDownstroke in the VKeyEvent
VKeyEvent linux_input_receive();

// Send a key event (PS/2 Set 1 scancode) via uinput
void linux_input_send(int ps2_scancode, bool is_down);

// Cleanup: destroy uinput device, close evdev devices
void linux_input_cleanup();

// Sleep for specified milliseconds
void linux_sleep_ms(int ms);

// Check if we are the foreground/active process (stub for Linux - ESC commands always work)
bool linux_is_foreground();

// Daemonize: detach from terminal and run in background
// Returns 0 on success, -1 on failure.
// Only the daemon child process ever returns from this function.
int linux_daemonize();