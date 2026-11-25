#pragma once

// The emulated gamepad of GameInput (gameinput.cpp) and its input sources.

#include <cstdint>
#include <string>
#include <vector>

namespace xbem::hle {

// GameInputGamepadButtons
enum GamepadButton : uint32_t {
  kGamepadMenu = 0x1,
  kGamepadView = 0x2,
  kGamepadA = 0x4,
  kGamepadB = 0x8,
  kGamepadX = 0x10,
  kGamepadY = 0x20,
  kGamepadDPadUp = 0x40,
  kGamepadDPadDown = 0x80,
  kGamepadDPadLeft = 0x100,
  kGamepadDPadRight = 0x200,
  kGamepadLeftShoulder = 0x400,
  kGamepadRightShoulder = 0x800,
  kGamepadLeftThumbstick = 0x1000,
  kGamepadRightThumbstick = 0x2000,
};

// A scripted press (`--press`): buttons held from `at_ms` after GameInputCreate
// for `hold_ms`.
struct GamepadPress {
  uint32_t buttons = 0;
  uint32_t at_ms = 0;
  uint32_t hold_ms = 200;
};

// Parses "A@20,MENU+A@25.5,B@30:1000" (buttons joined by '+', '@' seconds,
// optional ':' hold in milliseconds). Throws std::runtime_error on errors.
std::vector<GamepadPress> parse_gamepad_script(const std::string& spec);

// Presses (`down`) or releases buttons of the emulated gamepad (the run
// window's keyboard).
void gamepad_set_buttons(uint32_t buttons, bool down);

// The run window's keyboard (an X11 keysym): Enter/Space A, Backspace B,
// X, Y, arrows and WASD the D-pad, Escape Menu, Tab View, Q/E the shoulders.
void gamepad_key(unsigned long keysym, bool down);

}  // namespace xbem::hle
