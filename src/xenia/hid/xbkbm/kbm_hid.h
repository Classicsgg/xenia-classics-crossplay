/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_XBKBM_KBM_HID_H_
#define XENIA_HID_XBKBM_KBM_HID_H_

#include <memory>

#include "xenia/hid/input_system.h"

namespace xe {
namespace hid {
namespace xbkbm {

// Crossplay keyboard-and-mouse driver; see kbm_input_driver.h.
std::unique_ptr<InputDriver> Create(xe::ui::Window* window,
                                    size_t window_z_order);

}  // namespace xbkbm
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_XBKBM_KBM_HID_H_
