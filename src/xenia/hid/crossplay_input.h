/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_CROSSPLAY_INPUT_H_
#define XENIA_HID_CROSSPLAY_INPUT_H_

#include <cstdint>

// Crossplay input policy (RGH <> Xenia bridge, kernel/xbridge.h).
//
// A crossplay player is either on a controller (the default) or declared
// keyboard and mouse. The mode is read ONCE, the first time anything asks
// (at start-up, when the input drivers are created), and never changes for
// the life of the process: the player picks it in Netplay > Crossplay input
// and restarts Xenia. That latch is the enforcement:
//
//   controller  no keyboard or mouse input reaches the guest at all -- the
//               Canary keyboard driver is not created (app/xenia_main.cc).
//   kbm         the crossplay KBM driver (hid/xbkbm) is created: keys
//               mapped to pad buttons, a real pad still usable alongside,
//               and mouse aim written into the game's view angles. Aim assist
//               is removed from the game when its module loads
//               (kernel/xbridge_titles.cc). Mouse aim only runs once that
//               succeeded (the angle addresses belong to that exact build);
//               keys work everywhere, so a keyboard-only player can get
//               through the single-player xex's menus to Multiplayer.
//
// Both the mode and the aim-assist state go to the relay in every hello. The
// relay passes no game traffic for a KBM player whose aim assist is still on,
// so keyboard input with aim assist never reaches a crossplay match, and the
// host sees who is on keyboard and mouse and can refuse them.
namespace xe {
namespace hid {
namespace crossplay {

enum class Mode {
  kOff,         // bridge_enabled is off: stock Xenia input, no policy.
  kController,  // crossplay, controller only.
  kKbm,         // crossplay, keyboard and mouse declared.
};

// Latched on the first call.
Mode GetMode();
const char* ModeName(Mode mode);  // "off", "controller", "kbm"

// The mode saved for the next start (Netplay menu). Differs from GetMode()
// until Xenia is restarted.
Mode GetPendingMode();
void SetPendingMode(Mode mode);

// Aim-assist removal for KBM mode, set by the title's module load.
enum class AimAssist : uint32_t {
  kUnknown = 0,      // no supported module loaded yet
  kRemoved = 1,      // patched and read back: mouse aim live, hello says off
  kUnsupported = 2,  // a crossplay title, but not a build we know how to
                     // patch (e.g. the single-player xex): no mouse aim,
                     // and the relay refuses crossplay while KBM is declared
};
AimAssist GetAimAssist();
void SetAimAssist(AimAssist state);

// True only in KBM mode with aim assist removed: mouse aim runs, and the
// hello says aim_assist=off.
bool KbmInputLive();

}  // namespace crossplay
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_CROSSPLAY_INPUT_H_
