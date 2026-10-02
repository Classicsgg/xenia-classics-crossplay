/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_CROSSPLAY_CLIENT_H_
#define XENIA_HID_CROSSPLAY_CLIENT_H_

#include <cstdint>

#include "xenia/hid/input.h"

// Crossplay: the host's per-player settings, applied on the Xenia player's
// side -- what the ClassicsConsoleLite plugin does on a real 360.
//
// The host's mod menu pushes each player's choices as client dvars with the
// engine's own setClientDvar (`s cc_flipped "1"`), which reaches a Xenia
// guest like any other client. Lite polls them on a 360; this module polls
// the same dvars out of guest memory (the title's own dvar hash table) and
// applies them at the same layer Lite does:
//
//   cc_flipped   triggers <-> bumpers on the real pad (Lite's XamInputGetState
//                rewrite, threshold 30). Not on keyboard and mouse input, which
//                is bound by key, not by pad layout.
//   cc_senscomp  FOV -> turn-rate compensation (CoD4 + MW2): the stick turns
//                at the stock-FOV rate whatever cg_fov / cg_fovScale say.
//                CoD4 also gets Lite's aim-assist box correction for a raised
//                cg_fovScale, which Lite applies whether or not sens comp is on.
//   cc_layout    CoD4 Bumper Jumper (1) / Bumper Jumper Tactical (2), as bind
//                lines through the game's command buffer. Controller mode only:
//                the KBM key map assumes the Default layout.
//
// Everything runs on the guest thread that is polling the pad (XamInputGetState
// -> InputSystem::GetState), which is the game's main thread: the same thread
// that later builds the usercmd, so nothing here races the game. Inert unless
// crossplay is on (bridge_enabled) and the running module passes the title's
// build check.
namespace xe {
namespace hid {
namespace crossplay {

// The real pad's state, before keyboard and mouse input is merged into it.
void ApplyHostFlip(X_INPUT_GAMEPAD* pad);

// Once per guest pad poll: re-reads the host's settings (throttled) and
// applies the turn-rate compensation for the next usercmd.
void OnGuestPadPoll();

}  // namespace crossplay
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_CROSSPLAY_CLIENT_H_
