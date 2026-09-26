/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/crossplay_input.h"

#include <atomic>
#include <mutex>
#include <string>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

DECLARE_bool(bridge_enabled);

DEFINE_string(bridge_input_mode, "controller",
              "Crossplay input: \"controller\" (default) or \"kbm\" (keyboard "
              "and mouse; removes the game's aim assist and tells the host). "
              "Read at start-up only -- restart Xenia after changing it.",
              "Live");

namespace xe {
namespace hid {
namespace crossplay {
namespace {

Mode Parse(const std::string& value) {
  return value == "kbm" ? Mode::kKbm : Mode::kController;
}

std::atomic<uint32_t> aim_assist{uint32_t(AimAssist::kUnknown)};

}  // namespace

Mode GetMode() {
  static const Mode mode = [] {
    const Mode m =
        cvars::bridge_enabled ? Parse(cvars::bridge_input_mode) : Mode::kOff;
    if (m != Mode::kOff && cvars::bridge_input_mode != "controller" &&
        cvars::bridge_input_mode != "kbm") {
      XELOGW("Crossplay: unknown bridge_input_mode \"{}\", using controller",
             cvars::bridge_input_mode);
    }
    XELOGI("Crossplay: input mode {} (fixed until Xenia restarts)",
           ModeName(m));
    return m;
  }();
  return mode;
}

const char* ModeName(Mode mode) {
  switch (mode) {
    case Mode::kController:
      return "controller";
    case Mode::kKbm:
      return "kbm";
    default:
      return "off";
  }
}

Mode GetPendingMode() { return Parse(cvars::bridge_input_mode); }

void SetPendingMode(Mode mode) {
  OVERRIDE_string(bridge_input_mode,
                  mode == Mode::kKbm ? "kbm" : "controller");
}

AimAssist GetAimAssist() { return AimAssist(aim_assist.load()); }

void SetAimAssist(AimAssist state) { aim_assist.store(uint32_t(state)); }

bool KbmInputLive() {
  return GetMode() == Mode::kKbm && GetAimAssist() == AimAssist::kRemoved;
}

}  // namespace crossplay
}  // namespace hid
}  // namespace xe
