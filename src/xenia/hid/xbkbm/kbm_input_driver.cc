/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/xbkbm/kbm_input_driver.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"
#include "xenia/hid/crossplay_input.h"
#include "xenia/hid/input.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/memory.h"
#include "xenia/ui/ui_event.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

DEFINE_double(kbm_sensitivity, 1.0,
              "Crossplay keyboard-and-mouse: base mouse sensitivity. The "
              "game's own Look Sensitivity setting multiplies it (the "
              "game's default = x1).",
              "Live");
DEFINE_double(kbm_fov_sensitivity, 0.9,
              "Crossplay keyboard-and-mouse: how strongly aiming down sights "
              "(lower FOV) slows the mouse. 1 = in proportion to the zoom.",
              "Live");
DEFINE_bool(kbm_invert_y, false, "Crossplay keyboard-and-mouse: invert mouse Y.",
            "Live");
DEFINE_path(kbm_bindings_file, "crossplay_bindings.ini",
            "Crossplay keyboard-and-mouse key bindings (Key = Button lines, "
            "same names as mousehook's bindings.ini). Missing file = the "
            "built-in Call of Duty defaults.",
            "Live");

namespace xe {
namespace hid {
namespace xbkbm {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kWheelPress = std::chrono::milliseconds(60);

// ---- Mouse aim targets -----------------------------------------------------
// Where each supported title keeps local client 0's view angles. The build is
// NOT checked here: mouse aim runs only while KbmInputLive(), which is only
// true after the kernel matched this exact build and removed its aim assist
// (kernel/xbridge_titles.cc), so the pointers below are only ever used on the
// build they were read from.
struct AimTarget {
  uint32_t title_id;
  uint32_t client_ptr;     // guest address holding the client struct pointer
  uint32_t yaw_offset;     // pitch is the float before it
  uint32_t fov_static;     // FOV sensitivity scale at a fixed address, or 0
  uint32_t fov_offset;     // ...else at client + this offset
  // The game's Look Sensitivity: gamerSettings[0].viewSensitivity, the
  // profile mirror of input_viewSensitivity (medium = 1.0, slider top 4.0).
  // Same field the host plugin's KBM mode reads (Games/<T>/Addresses.h).
  uint32_t view_sensitivity;
};

constexpr AimTarget kAimTargets[] = {
    // Call of Duty 4 TU4 MP -- mousehook's CallOfDuty4_TU4_MP entry (clients
    // pointer; the debug-472 rosetta's `clients` is at the TU0 address
    // mousehook uses for TU0, with TU4 shifted by +4 as everywhere in TU4).
    // gamerSettings 0x84C01908 (stride 2912), viewSensitivity +0x04.
    {0x415607E6, 0x82435AB8, 0x25534, 0x84CD7D44, 0, 0x84C0190C},
    // Modern Warfare 2 TU9 MP (not in mousehook, which only has TU0). From
    // the TU9 xex: CL_AdjustAngles 0x8213E980 reaches the clients array via
    // *0x82682350 (stride 0x6D00) and turns viewangles yaw +0x335C / pitch
    // +0x3358; CL_SetFOVSensitivityScale 0x82138088 stores +0x3318.
    // gamerSettings 0x82686B38 (stride 2240), viewSensitivity +0x04.
    {0x41560817, 0x82682350, 0x335C, 0, 0x3318, 0x82686B3C},
};

const AimTarget* FindAimTarget(uint32_t title_id) {
  for (const AimTarget& t : kAimTargets) {
    if (t.title_id == title_id) {
      return &t;
    }
  }
  return nullptr;
}

// ---- Bindings -------------------------------------------------------------
// mousehook's "Call Of Duty 4" / "Modern Warfare 2" sections (bindings.ini),
// which assume the game's Default button layout.
constexpr const char* kDefaultBindings[] = {
    "Escape = Start", "W = LS-Up",    "S = LS-Down", "A = LS-Left",
    "D = LS-Right",   "Space = A",    "R = X",       "E = X",
    "F = X",          "1 = Y",        "2 = Left",    "3 = Right",
    "5 = Up",         "6 = Down",     "C = B",       "LClick = RT",
    "RClick = LT",    "MWheelUp = Y", "MWheelDown = Y", "MClick = RB",
    "Mouse4 = LB",    "Enter = Start", "Tab = Back",  "4 = LB",
    "G = RB",         "Shift = LS",   "V = RS",
};

const std::map<std::string, uint16_t>& KeyNames() {
  static const std::map<std::string, uint16_t> names = {
      {"lclick", VK_LBUTTON},   {"mouse1", VK_LBUTTON},
      {"rclick", VK_RBUTTON},   {"mouse2", VK_RBUTTON},
      {"mclick", VK_MBUTTON},   {"mouse3", VK_MBUTTON},
      {"mouse4", VK_XBUTTON1},  {"mouse5", VK_XBUTTON2},
      {"mwheelup", KbmInputDriver::kWheelUp},
      {"mwheeldown", KbmInputDriver::kWheelDown},
      {"shift", VK_SHIFT},      {"lshift", VK_SHIFT},    {"rshift", VK_SHIFT},
      {"ctrl", VK_CONTROL},     {"control", VK_CONTROL}, {"lctrl", VK_CONTROL},
      {"rctrl", VK_CONTROL},    {"alt", VK_MENU},        {"lalt", VK_MENU},
      {"ralt", VK_MENU},        {"capslock", VK_CAPITAL},
      {"space", VK_SPACE},      {"enter", VK_RETURN},    {"return", VK_RETURN},
      {"tab", VK_TAB},          {"escape", VK_ESCAPE},   {"backspace", VK_BACK},
      {"up", VK_UP},            {"down", VK_DOWN},       {"left", VK_LEFT},
      {"right", VK_RIGHT},      {"insert", VK_INSERT},   {"delete", VK_DELETE},
      {"home", VK_HOME},        {"end", VK_END},         {"pgup", VK_PRIOR},
      {"pgdown", VK_NEXT},      {";", VK_OEM_1},         {"=", VK_OEM_PLUS},
      {",", VK_OEM_COMMA},      {"-", VK_OEM_MINUS},     {".", VK_OEM_PERIOD},
      {"/", VK_OEM_2},          {"`", VK_OEM_3},         {"[", VK_OEM_4},
      {"\\", VK_OEM_5},         {"]", VK_OEM_6},         {"'", VK_OEM_7},
      {"f1", VK_F1},  {"f2", VK_F2},  {"f3", VK_F3},  {"f4", VK_F4},
      {"f5", VK_F5},  {"f6", VK_F6},  {"f7", VK_F7},  {"f8", VK_F8},
      {"f9", VK_F9},  {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},
      {"num0", VK_NUMPAD0}, {"num1", VK_NUMPAD1}, {"num2", VK_NUMPAD2},
      {"num3", VK_NUMPAD3}, {"num4", VK_NUMPAD4}, {"num5", VK_NUMPAD5},
      {"num6", VK_NUMPAD6}, {"num7", VK_NUMPAD7}, {"num8", VK_NUMPAD8},
      {"num9", VK_NUMPAD9},
  };
  return names;
}

std::string Trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  size_t b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

// "Key = Button[+Button...]". Returns false for anything unparsable.
bool ParseLine(const std::string& raw, uint16_t* key,
               KbmInputDriver::Binding* bind) {
  const std::string line = Trim(raw);
  if (line.empty() || line[0] == ';' || line[0] == '[') {
    return false;
  }
  const size_t eq = line.rfind('=');
  if (eq == std::string::npos || eq == 0) {
    return false;
  }
  const std::string key_name = Lower(Trim(line.substr(0, eq)));
  if (key_name.size() == 1 && std::isalnum(uint8_t(key_name[0]))) {
    *key = uint16_t(std::toupper(uint8_t(key_name[0])));
  } else if (auto it = KeyNames().find(key_name); it != KeyNames().end()) {
    *key = it->second;
  } else {
    return false;
  }

  static const std::map<std::string, uint16_t> buttons = {
      {"a", X_INPUT_GAMEPAD_A},
      {"b", X_INPUT_GAMEPAD_B},
      {"x", X_INPUT_GAMEPAD_X},
      {"y", X_INPUT_GAMEPAD_Y},
      {"lb", X_INPUT_GAMEPAD_LEFT_SHOULDER},
      {"rb", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
      {"ls", X_INPUT_GAMEPAD_LEFT_THUMB},
      {"rs", X_INPUT_GAMEPAD_RIGHT_THUMB},
      {"start", X_INPUT_GAMEPAD_START},
      {"back", X_INPUT_GAMEPAD_BACK},
      {"up", X_INPUT_GAMEPAD_DPAD_UP},
      {"down", X_INPUT_GAMEPAD_DPAD_DOWN},
      {"left", X_INPUT_GAMEPAD_DPAD_LEFT},
      {"right", X_INPUT_GAMEPAD_DPAD_RIGHT},
  };
  KbmInputDriver::Binding b;
  std::string token;
  const std::string value = Lower(line.substr(eq + 1)) + " ";
  for (char c : value) {
    if (c == ' ' || c == '+' || c == '\t') {
      if (token.empty()) {
        continue;
      }
      if (auto it = buttons.find(token); it != buttons.end()) {
        b.buttons |= it->second;
      } else if (token == "lt") {
        b.left_trigger = true;
      } else if (token == "rt") {
        b.right_trigger = true;
      } else if (token == "ls-up") {
        b.ly = 1;
      } else if (token == "ls-down") {
        b.ly = -1;
      } else if (token == "ls-left") {
        b.lx = -1;
      } else if (token == "ls-right") {
        b.lx = 1;
      }
      // Right-stick keys, modifiers and weapon macros are deliberately not
      // supported: aim comes from the mouse only.
      token.clear();
    } else {
      token += c;
    }
  }
  *bind = b;
  return true;
}

}  // namespace

// ---- Window listener -------------------------------------------------------

void KbmInputDriver::Listener::OnKeyDown(ui::KeyEvent& e) {
  const uint16_t vk = uint16_t(e.virtual_key());
  if (vk < 256) {
    driver_.keys_[vk].store(true);
  }
}

void KbmInputDriver::Listener::OnKeyUp(ui::KeyEvent& e) {
  const uint16_t vk = uint16_t(e.virtual_key());
  if (vk < 256) {
    driver_.keys_[vk].store(false);
  }
}

// ---- Driver ----------------------------------------------------------------

KbmInputDriver::KbmInputDriver(xe::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order), listener_(*this) {
  LoadBindings();
  window->AddInputListener(&listener_, window_z_order);
  // Raw input registration belongs to the window's thread.
  window->app_context().CallInUIThread([this, window]() {
    window->SetRawMouseSink(
        [this](int32_t dx, int32_t dy, uint16_t flags, int16_t wheel) {
          OnRawMouse(dx, dy, flags, wheel);
        });
  });
  XELOGI("Crossplay: keyboard and mouse driver ready");
}

KbmInputDriver::~KbmInputDriver() {
  window()->RemoveInputListener(&listener_);
  window()->SetRawMouseSink(nullptr);
}

void KbmInputDriver::LoadBindings() {
  bindings_ = {};
  int loaded = 0;
  std::ifstream file(cvars::kbm_bindings_file);
  if (file) {
    std::string line;
    while (std::getline(file, line)) {
      uint16_t key;
      Binding bind;
      if (ParseLine(line, &key, &bind) && key < bindings_.size()) {
        bindings_[key] = bind;
        loaded++;
      }
    }
  }
  if (loaded) {
    XELOGI("Crossplay: {} key bindings from {}", loaded,
           xe::path_to_utf8(cvars::kbm_bindings_file));
    return;
  }
  for (const char* line : kDefaultBindings) {
    uint16_t key;
    Binding bind;
    if (ParseLine(line, &key, &bind)) {
      bindings_[key] = bind;
    }
  }
  XELOGI("Crossplay: built-in Call of Duty key bindings");
}

X_STATUS KbmInputDriver::Setup() { return X_STATUS_SUCCESS; }

X_RESULT KbmInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  if (user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::memset(out_caps, 0, sizeof(*out_caps));
  out_caps->type = X_INPUT_DEVTYPE::XINPUT_DEVTYPE_GAMEPAD;
  out_caps->sub_type = X_INPUT_DEVSUBTYPE::XINPUT_DEVSUBTYPE_GAMEPAD;
  out_caps->gamepad.buttons = 0xFFFF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = int16_t(0xFFFFu);
  out_caps->gamepad.thumb_ly = int16_t(0xFFFFu);
  out_caps->gamepad.thumb_rx = int16_t(0xFFFFu);
  out_caps->gamepad.thumb_ry = int16_t(0xFFFFu);
  return X_ERROR_SUCCESS;
}

bool KbmInputDriver::IsDown(uint16_t key, Clock::time_point now) const {
  const int64_t t = now.time_since_epoch().count();
  if (key == kWheelUp) {
    return t < wheel_up_until_.load();
  }
  if (key == kWheelDown) {
    return t < wheel_down_until_.load();
  }
  return keys_[key].load();
}

X_RESULT KbmInputDriver::GetState(uint32_t user_index,
                                  X_INPUT_STATE* out_state) {
  // This driver only exists in declared KBM mode (app/xenia_main.cc).
  if (user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_INPUT_GAMEPAD pad = {};
  int32_t dx = 0, dy = 0;
  {
    std::lock_guard<std::mutex> lock(mouse_mutex_);
    std::swap(dx, mouse_dx_);
    std::swap(dy, mouse_dy_);
  }

  if (window()->HasFocus()) {
    const auto now = Clock::now();
    uint16_t buttons = 0;
    int lx = 0, ly = 0;
    bool lt = false, rt = false;
    for (uint16_t key = 0; key < bindings_.size(); key++) {
      const Binding& b = bindings_[key];
      if ((!b.buttons && !b.left_trigger && !b.right_trigger && !b.lx &&
           !b.ly) ||
          !IsDown(key, now)) {
        continue;
      }
      buttons |= b.buttons;
      lt |= b.left_trigger;
      rt |= b.right_trigger;
      lx += b.lx;
      ly += b.ly;
    }
    pad.buttons = buttons;
    pad.left_trigger = lt ? 0xFF : 0;
    pad.right_trigger = rt ? 0xFF : 0;
    pad.thumb_lx = int16_t(lx > 0 ? SHRT_MAX : lx < 0 ? SHRT_MIN : 0);
    pad.thumb_ly = int16_t(ly > 0 ? SHRT_MAX : ly < 0 ? SHRT_MIN : 0);
    // Mouse aim only on the exact build whose aim assist was removed.
    if ((dx || dy) && crossplay::KbmInputLive()) {
      ApplyMouseAim(dx, dy);
    }
  } else {
    // Nothing sticks down across an alt-tab.
    for (auto& k : keys_) {
      k.store(false);
    }
  }

  if (std::memcmp(&pad, &last_pad_, sizeof(pad)) != 0) {
    last_pad_ = pad;
    packet_number_++;
  }
  // Always "connected" (neutral when unfocused), so a KBM-only player never
  // gets the game's controller-disconnected pause on alt-tab.
  out_state->packet_number = packet_number_;
  out_state->gamepad = pad;
  return X_ERROR_SUCCESS;
}

void KbmInputDriver::ApplyMouseAim(int32_t dx, int32_t dy) {
  auto* kernel = xe::kernel::kernel_state();
  if (!kernel) {
    return;
  }
  const AimTarget* target = FindAimTarget(kernel->title_id());
  if (!target) {
    return;
  }
  Memory* memory = kernel->memory();
  const uint32_t client =
      *memory->TranslateVirtual<xe::be<uint32_t>*>(target->client_ptr);
  if (!client) {
    return;  // not in a game
  }
  auto* yaw = memory->TranslateVirtual<xe::be<float>*>(client +
                                                        target->yaw_offset);
  auto* pitch = memory->TranslateVirtual<xe::be<float>*>(
      client + target->yaw_offset - 4);
  const uint32_t fov_address = target->fov_static
                                   ? target->fov_static
                                   : client + target->fov_offset;
  float fov_scale = *memory->TranslateVirtual<xe::be<float>*>(fov_address);

  // mousehook's CoD scaling, unchanged: its feel is what players know.
  if (!(fov_scale > 0.f && fov_scale <= 1.f)) {
    fov_scale = 1.f;
  }
  const float a = float(cvars::kbm_fov_sensitivity);
  if (fov_scale != 1.f) {
    fov_scale = (1 - a) * (fov_scale * fov_scale) + a * fov_scale;
  }
  const float divisor = 10.5f / fov_scale;
  // The in-game slider scales the mouse, so players tune it where they
  // expect to (and without restarting Xenia). Anything implausible -- a
  // profile not loaded yet -- counts as the default.
  float game_sens =
      *memory->TranslateVirtual<xe::be<float>*>(target->view_sensitivity);
  if (!(game_sens > 0.05f && game_sens <= 10.f)) {
    game_sens = 1.f;
  }
  const float sens = float(cvars::kbm_sensitivity) * game_sens;

  *yaw = float(*yaw) - (float(dx) / divisor) * sens;
  const float dpitch = (float(dy) / divisor) * sens;
  *pitch = float(*pitch) + (cvars::kbm_invert_y ? -dpitch : dpitch);
}

X_RESULT KbmInputDriver::SetState(uint32_t user_index,
                                  X_INPUT_VIBRATION* vibration) {
  return user_index == 0 ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT KbmInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}

InputType KbmInputDriver::GetInputType() const { return InputType::Controller; }

void KbmInputDriver::OnRawMouse(int32_t dx, int32_t dy, uint16_t flags,
                                int16_t wheel) {
  {
    std::lock_guard<std::mutex> lock(mouse_mutex_);
    mouse_dx_ += dx;
    mouse_dy_ += dy;
  }
  struct {
    uint16_t down, up, vk;
  } constexpr kButtons[] = {
      {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, VK_LBUTTON},
      {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, VK_RBUTTON},
      {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, VK_MBUTTON},
      {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, VK_XBUTTON1},
      {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, VK_XBUTTON2},
  };
  for (const auto& b : kButtons) {
    if (flags & b.down) {
      keys_[b.vk].store(true);
    }
    if (flags & b.up) {
      keys_[b.vk].store(false);
    }
  }
  if (wheel) {
    const int64_t until = (Clock::now() + kWheelPress).time_since_epoch().count();
    (wheel > 0 ? wheel_up_until_ : wheel_down_until_).store(until);
  }
}

}  // namespace xbkbm
}  // namespace hid
}  // namespace xe
