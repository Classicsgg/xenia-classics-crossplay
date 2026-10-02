/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/crossplay_client.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>

#include "xenia/base/logging.h"
#include "xenia/hid/crossplay_input.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xbridge.h"
#include "xenia/memory.h"

namespace xe {
namespace hid {
namespace crossplay {
namespace {

using Clock = std::chrono::steady_clock;

// ---- Per-title addresses ----------------------------------------------------
// Every block is guarded by a build check: the words that must be at a
// function the block depends on (0 = don't care, a relative `bl`). The
// single-player xex shares the title ID, so a failed check means "not the
// multiplayer image this was written for" and the block does nothing.
struct ClientTitle {
  uint32_t title_id;

  // Dvar lookup = the title's own Dvar_FindVar walk: bucket =
  // sum(tolower(c) * (119 + i)) & hash_mask, chain through dvar+next.
  uint32_t find_var;  // the function the table addresses were read from
  uint32_t find_var_expect[8];
  uint32_t hash_table;  // dvar_t* buckets[]
  uint32_t hash_mask;
  uint32_t dvar_next;   // dvar_t* hashNext
  uint32_t dvar_type;   // u8 dvarType_t
  uint32_t dvar_value;  // current

  // Turn-rate compensation. AimAssist_FovScale (run while drawing a frame)
  // writes aaGlob.fovTurnRateScale = tanHalfFovY * 2.0929139 and
  // fovScaleInv = tan(cg_fov/2) * 0.75 / tanHalfFovY; ApplyTurnRates (from
  // the usercmd build) multiplies both stick rates by fovTurnRateScale.
  uint32_t fov_scale;  // AimAssist_FovScale, the build check
  uint32_t fov_scale_expect[13];
  uint32_t aa_glob;     // aaGlobArray[0]
  uint32_t turn_scale;  // fovTurnRateScale
  uint32_t box_scale;   // fovScaleInv
  uint32_t ads_lerp;    // adsLerp, 0 hip .. 1 full ADS
  bool box_fairness;    // Lite corrects fovScaleInv for cg_fovScale (CoD4)

  // Controller layout (cc_layout); 0 = not in this title.
  uint32_t profile;      // gamerSettings[0]
  uint32_t buttons_cfg;  // char[] buttons config name
  uint32_t sticks_cfg;   // char[] thumbstick config name
};

// CoD4 TU4 MP (retail IDB, 2026-10-02):
//   Dvar_FindVar 0x821D4C10: 256 buckets at 0x84B36038, hashNext +0x44, type
//     +0x0A, current +0x0C (Dvar_GetString 0x821D1478 reads the same).
//   AimAssist_FovScale 0x82347AE8 (only caller UpdateScreenTargets 0x82348E90,
//     from CG_DrawActiveFrame 0x822F53F8): +0x60 / +0x64. aaGlobArray
//     0x823A7408, adsLerp +0x68.
//   GamerProfile_ExecControllerBindings 0x821B3F28 execs the configs named at
//     gamerSettings 0x84C01908 + 0x09 (buttons) / + 0x109 (sticks).
// MW2 TU9 MP (TU9 IDB + Alpha 482 rosetta, 2026-10-02):
//   Dvar_FindMalleableVar 0x8229ED20: 1024 buckets at 0x8374D590, hashNext
//     +0x40, type +0x06, current +0x08 (Dvar_GetString 0x8229F0A8).
//   AimAssist_FovScale 0x820C3DC0 (UpdateScreenTargets 0x820C4540 from
//     CG_DrawActiveFrame 0x82129794): +0x118 / +0x11C. aaGlobArray 0x82546780,
//     adsLerp +0x120 (AimAssist_UpdateAdsLerp 0x820C3B80).
// Order, both titles: the pad poll runs before the usercmd build, and both
// run before UpdateScreenTargets in the same frame (CoD4 0x822F5200 /
// 0x822F53F8, MW2 0x821294FC / 0x82129794; Com_Frame polls before CL_Frame
// too), so a turn scale written here is the one the usercmd reads, and the
// game puts its own value back when it draws.
constexpr ClientTitle kTitles[] = {
    {0x415607E6,
     0x821D4C10,
     {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x3D6084B3,
      0x7C7F1B78, 0x3BCB6038},
     0x84B36038, 0xFF, 0x44, 0x0A, 0x0C,
     0x82347AE8,
     {0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0, 0xDBE1FFE8, 0x9421FF90, 0x3D608209,
      0xFFE00890, 0x7C7F1B78, 0xC00BC648, 0x3D60823F, 0x816B5784, 0xEC1F0032,
      0xD01F0060},  // ... stfs f0, 0x60(r31)
     0x823A7408, 0x60, 0x64, 0x68, true,
     0x84C01908, 0x09, 0x109},
    {0x41560817,
     0x8229ED20,
     {0x7D8802A6, 0, 0x9421FF90, 0x3D608375, 0x7C7E1B78, 0x3BABD590,
      0x387DBFF4, 0},
     0x8374D590, 0x3FF, 0x40, 0x06, 0x08,
     0x820C3DC0,
     {0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0, 0xDBE1FFE8, 0x9421FF90, 0x3D608200,
      0xFFE00890, 0x3D40825C, 0x3D208200, 0x3D008200, 0x7C7F1B78, 0xC00B09E4,
      0xEC010032},  // next: stfs f0, 0x118(r3)
     0x82546780, 0x118, 0x11C, 0x120, false,
     0, 0, 0},
};

// dvarType_t (IW3 and IW4 alike).
constexpr uint8_t kDvarBool = 0;
constexpr uint8_t kDvarFloat = 1;
constexpr uint8_t kDvarInt = 5;
constexpr uint8_t kDvarString = 7;

constexpr auto kPollInterval = std::chrono::milliseconds(100);
// The layout lives in the live key table, which anything that re-execs a
// controller config puts back; Lite re-asserts it on the same 10 s beat.
constexpr auto kLayoutReassert = std::chrono::seconds(10);
constexpr auto kLayoutRetry = std::chrono::seconds(2);

constexpr float kBaseFov = 65.0f;
constexpr float kTurnRateK = 2.0929139f;  // 1 / (tan(65/2) * 0.75)
constexpr float kVFovRatio = 0.75f;

// The guest's pad poll is single-threaded (the title's main thread), but the
// flip is read by whichever thread asks for the pad, so it is atomic.
std::atomic<bool> flip_{false};

struct State {
  const ClientTitle* title = nullptr;
  uint32_t title_id = 0;
  bool dvars_ok = false;
  bool fov_ok = false;
  Clock::time_point next_poll{};

  // Last host values (-1 = dvar not set).
  int senscomp = -1;
  int layout = -1;
  int flipped = -1;

  // Cached dvar_t* by name; re-validated by name on every use.
  struct Cached {
    const char* name;
    uint32_t dvar;
  };
  Cached cg_fov{"cg_fov", 0};
  Cached cg_fov_scale{"cg_fovScale", 0};
  Cached cc_flipped{"cc_flipped", 0};
  Cached cc_senscomp{"cc_senscomp", 0};
  Cached cc_layout{"cc_layout", 0};

  float fov = kBaseFov;
  float fov_scale = 1.0f;

  // A field this module wrote, so a value still sitting there from the last
  // poll (no frame drawn since) is recognised and never scaled twice.
  struct Tracked {
    bool valid = false;
    float written = 0;
    float original = 0;
  };
  Tracked turn;
  Tracked box;

  int layout_applied = 0;
  Clock::time_point layout_time{};
  bool layout_kbm_logged = false;
};

State state_;
// Titles may poll the pad from a second thread (e.g. a loading screen drawn
// by the render thread), so the poll state is serialised.
std::mutex state_mutex_;

bool Readable(Memory* memory, uint32_t address, uint32_t size) {
  if (!address) {
    return false;
  }
  for (uint32_t a : {address, address + size - 1}) {
    BaseHeap* heap = memory->LookupHeap(a);
    uint32_t protect = 0;
    if (!heap || !heap->QueryProtect(a, &protect) ||
        !(protect & kMemoryProtectRead)) {
      return false;
    }
  }
  return true;
}

uint32_t ReadU32(Memory* memory, uint32_t address) {
  return *memory->TranslateVirtual<xe::be<uint32_t>*>(address);
}

float ReadFloat(Memory* memory, uint32_t address) {
  return *memory->TranslateVirtual<xe::be<float>*>(address);
}

void WriteFloat(Memory* memory, uint32_t address, float value) {
  *memory->TranslateVirtual<xe::be<float>*>(address) = value;
}

bool CodeMatches(Memory* memory, uint32_t address, const uint32_t* expect,
                 size_t count) {
  if (!Readable(memory, address, uint32_t(count * 4))) {
    return false;
  }
  for (size_t i = 0; i < count; i++) {
    if (expect[i] && ReadU32(memory, address + uint32_t(i * 4)) != expect[i]) {
      return false;
    }
  }
  return true;
}

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }

bool GuestNameIs(Memory* memory, uint32_t guest_str, const char* name) {
  const size_t len = std::strlen(name);
  if (!Readable(memory, guest_str, uint32_t(len + 1))) {
    return false;
  }
  const char* s = memory->TranslateVirtual<const char*>(guest_str);
  for (size_t i = 0; i <= len; i++) {
    if (Lower(s[i]) != Lower(name[i])) {
      return false;
    }
  }
  return true;
}

bool IsDvar(Memory* memory, const ClientTitle& t, uint32_t dvar,
            const char* name) {
  return Readable(memory, dvar, t.dvar_next + 4) &&
         GuestNameIs(memory, ReadU32(memory, dvar), name);
}

uint32_t FindDvar(Memory* memory, const ClientTitle& t, const char* name) {
  uint32_t hash = 0;
  for (uint32_t i = 0; name[i]; i++) {
    hash += uint32_t(uint8_t(Lower(name[i]))) * (119 + i);
  }
  const uint32_t bucket = t.hash_table + (hash & t.hash_mask) * 4;
  if (!Readable(memory, bucket, 4)) {
    return 0;
  }
  uint32_t dvar = ReadU32(memory, bucket);
  for (int guard = 0; dvar && guard < 256; guard++) {
    if (!Readable(memory, dvar, t.dvar_next + 4)) {
      return 0;
    }
    if (GuestNameIs(memory, ReadU32(memory, dvar), name)) {
      return dvar;
    }
    dvar = ReadU32(memory, dvar + t.dvar_next);
  }
  return 0;
}

uint32_t Lookup(Memory* memory, const ClientTitle& t, State::Cached& c) {
  if (c.dvar && IsDvar(memory, t, c.dvar, c.name)) {
    return c.dvar;
  }
  c.dvar = FindDvar(memory, t, c.name);
  return c.dvar;
}

// The host pushes strings ("1"), but a dvar the title registered itself keeps
// its own type, so every scalar type is read. -1 = not set / unreadable.
int DvarInt(Memory* memory, const ClientTitle& t, State::Cached& c) {
  const uint32_t dvar = Lookup(memory, t, c);
  if (!dvar || !Readable(memory, dvar + t.dvar_value, 4)) {
    return -1;
  }
  const uint32_t value = dvar + t.dvar_value;
  switch (*memory->TranslateVirtual<uint8_t*>(dvar + t.dvar_type)) {
    case kDvarBool:
      return *memory->TranslateVirtual<uint8_t*>(value) ? 1 : 0;
    case kDvarInt:
      return int32_t(ReadU32(memory, value));
    case kDvarFloat:
      return int(ReadFloat(memory, value));
    case kDvarString: {
      const uint32_t str = ReadU32(memory, value);
      if (!Readable(memory, str, 1)) {
        return -1;
      }
      const char* s = memory->TranslateVirtual<const char*>(str);
      int v = 0;
      int digits = 0;
      for (; digits < 6 && s[digits] >= '0' && s[digits] <= '9'; digits++) {
        v = v * 10 + (s[digits] - '0');
      }
      return digits ? v : -1;
    }
    default:
      return -1;
  }
}

// lo / hi are the dvar's own domain: a value outside it means this isn't the
// dvar we think it is, and the fallback is safer than any clamp.
float DvarFloat(Memory* memory, const ClientTitle& t, State::Cached& c,
                float lo, float hi, float fallback) {
  const uint32_t dvar = Lookup(memory, t, c);
  if (!dvar || !Readable(memory, dvar + t.dvar_value, 4) ||
      *memory->TranslateVirtual<uint8_t*>(dvar + t.dvar_type) != kDvarFloat) {
    return fallback;
  }
  const float v = ReadFloat(memory, dvar + t.dvar_value);
  return (v >= lo && v <= hi) ? v : fallback;
}

// ---- Turn-rate compensation (Lite: COD4/ClientMods.cpp, same formulas) ----

// cg_fovScale's share, undone, read off the live turn scale so it is exact at
// every point of the ADS blend. CG_GetViewFov multiplies the fov in DEGREES,
// so the angle is divided by s, not the tangent.
float FovScaleFactor(float turn_scale, float scale) {
  if (scale > 0.999f && scale < 1.001f) {
    return 1.0f;
  }
  if (turn_scale <= 0.0001f || turn_scale > 100.0f) {
    return 1.0f;
  }
  const float tan_half_x = (turn_scale / kTurnRateK) / kVFovRatio;
  if (tan_half_x <= 0.0001f || tan_half_x > 100.0f) {
    return 1.0f;
  }
  const float unscaled = std::tan(std::atan(tan_half_x) / scale);
  return unscaled > 0.0001f ? unscaled / tan_half_x : 1.0f;
}

// cg_fov's share, undone -- the hip end only (at full ADS the view fov is the
// weapon's, the same for everyone).
float FovFactor(float fov) {
  constexpr float kDegToRad = 0.0174532925f;
  const float actual = std::tan(fov * 0.5f * kDegToRad);
  const float base = std::tan(kBaseFov * 0.5f * kDegToRad);
  return (actual > 0.0001f && base > 0.0001f) ? base / actual : 1.0f;
}

// The field's own value: what the game last wrote, never our scaled copy.
bool Original(Memory* memory, uint32_t address, State::Tracked& t,
              float* out) {
  const float current = ReadFloat(memory, address);
  const float original =
      (t.valid && current == t.written) ? t.original : current;
  if (!(original > 0.0001f && original < 100.0f)) {
    return false;
  }
  *out = original;
  return true;
}

void Write(Memory* memory, uint32_t address, State::Tracked& t,
           float original, float value) {
  WriteFloat(memory, address, value);
  t.valid = true;
  t.original = original;
  t.written = ReadFloat(memory, address);  // compare as stored
}

// Puts the game's value back if ours is still there.
void Release(Memory* memory, uint32_t address, State::Tracked& t) {
  if (t.valid && ReadFloat(memory, address) == t.written) {
    WriteFloat(memory, address, t.original);
  }
  t.valid = false;
}

void ApplyTurnCompensation(Memory* memory, State& s) {
  const ClientTitle& t = *s.title;
  const uint32_t turn = t.aa_glob + t.turn_scale;
  const uint32_t box = t.aa_glob + t.box_scale;
  const bool scaled = s.fov_scale <= 0.999f || s.fov_scale >= 1.001f;
  const bool want_turn = s.senscomp == 1;
  const bool want_box = t.box_fairness && scaled;

  float turn_original = 0;
  const bool have_turn = Original(memory, turn, s.turn, &turn_original);

  if (want_turn && have_turn) {
    float ads = ReadFloat(memory, t.aa_glob + t.ads_lerp);
    ads = std::isfinite(ads) ? std::fmin(std::fmax(ads, 0.0f), 1.0f) : 0.0f;
    const float hip = FovFactor(s.fov);
    const float factor = FovScaleFactor(turn_original, s.fov_scale) *
                         (hip + (1.0f - hip) * ads);
    if (factor != 1.0f) {
      Write(memory, turn, s.turn, turn_original, turn_original * factor);
    } else {
      Release(memory, turn, s.turn);
    }
  } else {
    Release(memory, turn, s.turn);
  }

  // Aim-assist box: fovScaleInv shrunk by exactly the amount FovScaleFactor
  // un-shrinks the turn rate (Lite applies it whenever fovScale != 1).
  float box_original = 0;
  if (want_box && have_turn && Original(memory, box, s.box, &box_original)) {
    const float f = FovScaleFactor(turn_original, s.fov_scale);
    if (f > 0.0001f && f != 1.0f) {
      Write(memory, box, s.box, box_original, box_original / f);
    } else {
      Release(memory, box, s.box);
    }
  } else {
    Release(memory, box, s.box);
  }
}

// ---- Controller layout (CoD4) ---------------------------------------------

// A config name from the profile, as Lite's restore (the engine's own
// ExecControllerBindings) would exec it. Anything that isn't a plain name
// falls back to the default rather than going into the command buffer.
std::string ProfileConfig(Memory* memory, uint32_t address,
                          const char* fallback) {
  if (!Readable(memory, address, 64)) {
    return fallback;
  }
  const char* s = memory->TranslateVirtual<const char*>(address);
  std::string name;
  for (int i = 0; i < 63 && s[i]; i++) {
    const char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_')) {
      return fallback;
    }
    name += c;
  }
  return name.empty() ? std::string(fallback) : name;
}

// Lite (COD4/ClientMods.cpp ApplyLayout, Keyboard and Mouse off): the
// player's stick config, then buttons_default under the layout -- the A/LB
// lines assume A=+gostand / LB=+smoke, which only buttons_default guarantees
// -- then the fixed bind lines (retail buttons_*.cfg, verified 2026-09-15).
// Off: both of the player's own configs, which were never modified.
std::string LayoutCommand(Memory* memory, const ClientTitle& t, int layout) {
  const std::string sticks = ProfileConfig(
      memory, t.profile + t.sticks_cfg, "thumbstick_default");
  if (layout <= 0) {
    return "exec " +
           ProfileConfig(memory, t.profile + t.buttons_cfg, "buttons_default") +
           "\nexec " + sticks + "\n";
  }
  std::string text = "exec " + sticks +
                     "\nexec buttons_default\n"
                     "bind BUTTON_A \"+smoke\"\n"
                     "bind BUTTON_LSHLDR \"+gostand\"\n";
  if (layout == 2) {
    text +=
        "bind BUTTON_B \"+melee\"\n"
        "bind BUTTON_RSTICK \"+stance\"\n";
  }
  return text;
}

void ApplyLayout(Memory* memory, State& s, Clock::time_point now) {
  const ClientTitle& t = *s.title;
  if (!t.profile) {
    return;
  }
  int want = (s.layout >= 0 && s.layout <= 2) ? s.layout : 0;
  if (GetMode() == Mode::kKbm && want) {
    if (!s.layout_kbm_logged) {
      s.layout_kbm_logged = true;
      XELOGI(
          "Crossplay: the host's Bumper Jumper layout is not applied in "
          "keyboard and mouse mode (keys assume the Default layout)");
    }
    want = 0;
  }
  if (want != s.layout_applied) {
    if (now < s.layout_time) {
      return;  // a failed attempt, retried shortly
    }
    if (kernel::bridge::RunTitleCommand(LayoutCommand(memory, t, want))) {
      XELOGI("Crossplay: controller layout {} applied",
             want == 0 ? "of the profile"
                       : (want == 1 ? "Bumper Jumper" : "Bumper Jumper Tactical"));
      s.layout_applied = want;
      s.layout_time = now + kLayoutReassert;
    } else {
      s.layout_time = now + kLayoutRetry;
    }
    return;
  }
  if (s.layout_applied > 0 && now >= s.layout_time) {
    // Same layout again: idempotent bind lines, no key state touched.
    kernel::bridge::RunTitleCommand(LayoutCommand(memory, t, s.layout_applied));
    s.layout_time = now + kLayoutReassert;
  }
}

const ClientTitle* FindClientTitle(uint32_t title_id) {
  for (const ClientTitle& t : kTitles) {
    if (t.title_id == title_id) {
      return &t;
    }
  }
  return nullptr;
}

void Reset(State& s, uint32_t title_id) {
  s = State();
  s.title_id = title_id;
  s.title = FindClientTitle(title_id);
  flip_.store(false);
}

}  // namespace

void ApplyHostFlip(X_INPUT_GAMEPAD* pad) {
  if (!pad || !flip_.load(std::memory_order_relaxed)) {
    return;
  }
  // Lite's XamInputGetState rewrite, word for word.
  uint16_t buttons = pad->buttons;
  const bool lb = (buttons & X_INPUT_GAMEPAD_LEFT_SHOULDER) != 0;
  const bool rb = (buttons & X_INPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
  buttons &= ~uint16_t(X_INPUT_GAMEPAD_LEFT_SHOULDER |
                       X_INPUT_GAMEPAD_RIGHT_SHOULDER);
  if (pad->left_trigger > 30) {
    buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  }
  if (pad->right_trigger > 30) {
    buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  }
  pad->buttons = buttons;
  pad->left_trigger = lb ? 0xFF : 0x00;
  pad->right_trigger = rb ? 0xFF : 0x00;
}

void OnGuestPadPoll() {
  if (GetMode() == Mode::kOff) {
    return;
  }
  auto* kernel = kernel::kernel_state();
  if (!kernel) {
    return;
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  State& s = state_;
  const uint32_t title_id = kernel->title_id();
  if (title_id != s.title_id) {
    Reset(s, title_id);
  }
  if (!s.title) {
    return;
  }
  const ClientTitle& t = *s.title;
  Memory* memory = kernel->memory();

  const Clock::time_point now = Clock::now();
  if (now >= s.next_poll) {
    s.next_poll = now + kPollInterval;

    // Checked on every poll: the single-player and multiplayer images share
    // the title ID, and the guest moves between them without a title change.
    const bool dvars_ok = CodeMatches(memory, t.find_var, t.find_var_expect,
                                      std::size(t.find_var_expect));
    const bool fov_ok = dvars_ok &&
                        CodeMatches(memory, t.fov_scale, t.fov_scale_expect,
                                    std::size(t.fov_scale_expect));
    if (dvars_ok != s.dvars_ok || fov_ok != s.fov_ok) {
      XELOGI("Crossplay: host player settings {} (dvars {}, turn rate {})",
             dvars_ok ? "active" : "inactive", dvars_ok ? "found" : "not found",
             fov_ok ? "found" : "not found");
      if (!dvars_ok) {
        const uint32_t keep = s.title_id;
        Reset(s, keep);  // a new image: nothing cached is valid
      }
      s.dvars_ok = dvars_ok;
      s.fov_ok = fov_ok;
    }
    if (!s.dvars_ok) {
      return;
    }

    const int flipped = DvarInt(memory, t, s.cc_flipped);
    const int senscomp = DvarInt(memory, t, s.cc_senscomp);
    const int layout = t.profile ? DvarInt(memory, t, s.cc_layout) : -1;
    if (flipped != s.flipped || senscomp != s.senscomp || layout != s.layout) {
      XELOGI("Crossplay: host player settings flipped={} senscomp={} layout={}",
             flipped, senscomp, layout);
    }
    s.flipped = flipped;
    s.senscomp = senscomp;
    s.layout = layout;
    flip_.store(flipped == 1);

    // Domains are the dvars' own registrations (cg_fov 1..160, cg_fovScale
    // 0.2..2.0).
    s.fov = DvarFloat(memory, t, s.cg_fov, 1.0f, 160.0f, kBaseFov);
    s.fov_scale = DvarFloat(memory, t, s.cg_fov_scale, 0.2f, 2.0f, 1.0f);

    ApplyLayout(memory, s, now);
  }

  if (s.fov_ok) {
    ApplyTurnCompensation(memory, s);
  }
}

}  // namespace crossplay
}  // namespace hid
}  // namespace xe
