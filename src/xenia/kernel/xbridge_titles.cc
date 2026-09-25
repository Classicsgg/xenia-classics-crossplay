/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Per-title crossplay bridge hooks. Adding a title = one TitleHooks row plus
// whatever its join needs prepared locally; the transport in xbridge.cc does
// not change.

#include <cstdio>
#include <cstring>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xbridge.h"
#include "xenia/memory.h"

DECLARE_path(bridge_stats_file);

namespace xe {
namespace kernel {
namespace bridge {
namespace {

// ---- Call of Duty 4 (415607E6, TU4) ----------------------------------------
// Offline stats for a bridged client. A real 360 in our lobbies runs the Lite
// plugin, which fills the client's stats block and sets statsFetched; Xenia
// is never signed in to Live, so the block stays zero: the custom-class menu
// parks on "Downloading game settings" and the host receives an all-zero
// stats upload. The upload (CL_CheckForResend, retail sub_822D1870) starts
// right after connectResponse, so the OOB "connect" is the last safe point
// to fill controllerStatData[c] (retail 0x84C5E238, stride 0x421C: 8 KB
// playerStats, statsFetched at +0x4218) for every controller. Patch files
// run too early: the sign-in handlers and ReadStats clear the block.
void Cod4OnConnect() {
  if (cvars::bridge_stats_file.empty()) {
    return;
  }
  constexpr uint32_t kStatData = 0x84C5E238;
  constexpr uint32_t kStride = 0x421C;
  constexpr uint32_t kStatsFetched = 0x4218;
  constexpr size_t kStatsSize = 0x2000;

  std::vector<uint8_t> blob(kStatsSize);
  FILE* file = xe::filesystem::OpenFile(cvars::bridge_stats_file, "rb");
  const size_t read = file ? std::fread(blob.data(), 1, blob.size(), file) : 0;
  if (file) {
    std::fclose(file);
  }
  if (read != kStatsSize) {
    XELOGE("Bridge: {} is not an 8 KB .mpdata stats block",
           xe::path_to_utf8(cvars::bridge_stats_file));
    return;
  }

  Memory* memory = kernel_state()->memory();
  for (uint32_t c = 0; c < 4; c++) {
    uint8_t* block =
        memory->TranslateVirtual<uint8_t*>(kStatData + c * kStride);
    std::memcpy(block, blob.data(), kStatsSize);
    block[kStatsFetched] = 1;      // statsFetched
    block[kStatsFetched + 1] = 0;  // statWriteNeeded
  }
  XELOGI("Bridge: loaded CoD4 stats from {} before connect",
         xe::path_to_utf8(cvars::bridge_stats_file));
}

const TitleHooks kTitles[] = {
    {0x415607E6, "Call of Duty 4", Cod4OnConnect, nullptr},
};

}  // namespace

const TitleHooks* FindTitle(uint32_t title_id) {
  for (const TitleHooks& t : kTitles) {
    if (t.title_id == title_id) {
      return &t;
    }
  }
  return nullptr;
}

}  // namespace bridge
}  // namespace kernel
}  // namespace xe
