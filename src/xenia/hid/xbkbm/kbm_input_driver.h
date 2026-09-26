/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_XBKBM_KBM_INPUT_DRIVER_H_
#define XENIA_HID_XBKBM_KBM_INPUT_DRIVER_H_

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "xenia/hid/input_driver.h"
#include "xenia/ui/window_listener.h"

namespace xe {
namespace hid {
namespace xbkbm {

// Crossplay keyboard-and-mouse driver (see hid/crossplay_input.h). Created
// only when the latched crossplay mode is kbm. The mouse technique is the one
// xenia-canary-mousehook uses for Call of Duty: raw mouse motion is written
// straight into the game's view angles each time the guest polls the pad,
// with no stick emulation, deadzone or turn-rate cap. Keys map to pad
// buttons and full stick deflection.
//
// It merges with a real pad instead of competing with it
// (MergesWithOtherDrivers): a player can move on a pad and aim with the
// mouse. Mouse aim runs only while KbmInputLive() -- after this build's aim
// assist was removed and read back; keys work in any module (menus, the
// single-player xex), because the relay passes no crossplay traffic for a
// KBM player whose aim assist is still on.
class KbmInputDriver final : public InputDriver {
 public:
  KbmInputDriver(xe::ui::Window* window, size_t window_z_order);
  ~KbmInputDriver() override;

  X_STATUS Setup() override;
  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;
  InputType GetInputType() const override;
  bool MergesWithOtherDrivers() const override { return true; }

  // Key index for the wheel "keys" in the binding table (unused VK codes,
  // the same ones mousehook uses).
  static constexpr uint16_t kWheelUp = 0x0E;
  static constexpr uint16_t kWheelDown = 0x0F;

  struct Binding {
    uint16_t buttons = 0;
    bool left_trigger = false;
    bool right_trigger = false;
    int8_t lx = 0;  // -1, 0, +1
    int8_t ly = 0;
  };

 private:
  class Listener final : public ui::WindowInputListener {
   public:
    explicit Listener(KbmInputDriver& driver) : driver_(driver) {}
    void OnKeyDown(ui::KeyEvent& e) override;
    void OnKeyUp(ui::KeyEvent& e) override;

   private:
    KbmInputDriver& driver_;
  };

  void OnRawMouse(int32_t dx, int32_t dy, uint16_t button_flags,
                  int16_t wheel);
  void LoadBindings();
  bool IsDown(uint16_t key, std::chrono::steady_clock::time_point now) const;
  void ApplyMouseAim(int32_t dx, int32_t dy);

  Listener listener_;
  std::array<Binding, 256> bindings_ = {};
  std::array<std::atomic<bool>, 256> keys_ = {};

  // Wheel detents become a short button press (the guest polls at frame
  // rate; a single-poll press can be missed).
  std::atomic<int64_t> wheel_up_until_{0};
  std::atomic<int64_t> wheel_down_until_{0};

  std::mutex mouse_mutex_;
  int32_t mouse_dx_ = 0;  // accumulated since the last poll
  int32_t mouse_dy_ = 0;

  uint32_t packet_number_ = 1;
  X_INPUT_GAMEPAD last_pad_ = {};
};

}  // namespace xbkbm
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_XBKBM_KBM_INPUT_DRIVER_H_
