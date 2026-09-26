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

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xbridge.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

DECLARE_path(bridge_stats_file);
DECLARE_bool(bridge_mw2_unlock_all);

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

// ---- Aim assist removal (crossplay KBM) ------------------------------------
// See AimAssistPatch in xbridge.h. Both functions are void, called only from
// AimAssist_UpdateScreenTargets (from CG_DrawActiveFrame), which zeroes the
// target count every frame before adding; every aim-assist consumer reads
// that list (via AimAssist_GetBestTarget / GetPrevOrBestTarget).
//
// CoD4 TU4 MP: AimAssistGlobals stride 3636, screenTargetCount +0xDFC.
// Debug-472 rosetta AimAssist_AddToTargetList is 0x8233E948; every aim-assist
// function around it sits exactly +0x8850 higher in retail TU4 (UpdateTweakables
// 0x82347B70, ApplyTurnRates 0x82349CD0, ApplyAutoAim 0x82348228,
// ApplyAutoMelee 0x82348028, ApplyLockOn 0x82347D58, ApplyMeleeCharge
// 0x82347448 -- the callees of retail AimAssist_UpdateGamePadInput
// 0x82349FC8). The expect words are the rosetta's body through its load of
// screenTargetCount; a mismatch leaves the game alone and KBM off.
// TODO(crossplay): confirm in the retail TU4 IDB (not yet read directly).
constexpr AimAssistPatch kCod4AimAssist = {
    0x82347198,
    {0x7D8802A6,    // mflr r12
     0,             // bl __savegprlr_29 (relative)
     0x9421FF90,    // stwu r1, -0x70(r1)
     0x7C7F1B78,    // mr r31, r3        aaGlob
     0x7C9D2378,    // mr r29, r4        screenTarget
     0x39600000,    // li r11, 0
     0x80FF0DFC,    // lwz r7, 0xDFC(r31)  -- screenTargetCount
     0}};

// MW2 TU9 MP: sub_820C4390 (only caller: the screen-target update at
// 0x820C4540, which stores the count 0 at 0x820C45FC), screenTargetCount
// +0xE34. Alpha 482 name: AimAssist_AddToTargetList.
constexpr AimAssistPatch kMw2AimAssist = {
    0x820C4390,
    {0x7D8802A6,    // mflr r12
     0x482DF379,    // bl __savegprlr_29
     0x9421FF90,    // stwu r1, -0x70(r1)
     0x80C30E34,    // lwz r6, 0xE34(r3)  -- screenTargetCount
     0x7C7F1B78,    // mr r31, r3
     0x7C9D2378,    // mr r29, r4
     0, 0}};

// ---- Modern Warfare 2 (41560817, TU9): max level + unlock all offline ------
// A Xenia player joins through System Link, so MW2 uses the profile's OFFLINE
// stats (LiveStorage_GetActiveStatsSource: onlinegame 0, systemlink 1 ->
// STATS_OFFLINE), saved in Xenia's content folder. A fresh profile is level 1:
// Create-a-Class opens at level 4 (unlockTable "cac" rank 3), the in-game
// class menu shows a custom class only once its `inUse` is set by the editor,
// and the host builds loadouts through isItemUnlocked on the stats the client
// uploads -- so a Xenia player spawned with default classes only. This is the
// Xenia side of what the CoD4 Lite plugin does for a new system-link profile:
// the offline stats are raised to max level with every challenge complete,
// then the player builds classes in System Link > Create a Class before
// joining the host.
//
// Values = MW2's own dev completeAllChallenges(1.0) (_missions.gsc):
// challengeState[ref] = tiers + 1, challengeProgress[ref] = final target;
// experience = the rank table's max; calling-card icon/title and killstreak
// unlocked bits, minus what unlockTable disables (rank 9999). Only raised,
// never lowered, and only while the buffer's def version and format checksum
// match the table's. MW2 saves it (statWriteNeeded) with its own checksum.
//
// TU9 controllerStatData 0x831A05C0: 0x6230 per controller, 0x2004 per stats
// source (0 online, 1 offline): u32 checksum, 8188 bytes little-endian
// structured data (version, format checksum, fields...), statsFetched
// +0x2000, statWriteNeeded +0x2001 (LiveStorage_StatsWriteNeeded 0x82229B60).
#include "xenia/kernel/xbridge_mw2_unlock.inc"

bool Mw2ClientDisconnected();
void Mw2SyncFakeLiveStats();

void Mw2RaiseOfflineProfile();

void Mw2OnTick() {
  if (cvars::bridge_mw2_unlock_all) {
    Mw2RaiseOfflineProfile();
  }
  if (XLiveAPI::BridgeFakeLive()) {
    Mw2SyncFakeLiveStats();
  }
}

void Mw2RaiseOfflineProfile() {
  constexpr uint32_t kStatData = 0x831A05C0;
  constexpr uint32_t kControllerStride = 0x6230;
  constexpr uint32_t kSourceStride = 0x2004;
  constexpr uint32_t kOffline = 1;
  constexpr uint32_t kFetched = 0x2000;
  constexpr uint32_t kWriteNeeded = 0x2001;
  static bool warned = false;

  Memory* memory = kernel_state()->memory();
  for (uint32_t c = 0; c < 4; c++) {
    uint8_t* src = memory->TranslateVirtual<uint8_t*>(
        kStatData + c * kControllerStride + kOffline * kSourceStride);
    if (!src[kFetched]) {
      continue;
    }
    uint8_t* data = src + 4;
    if (xe::load<uint32_t>(data) != kMw2DefVersion ||
        xe::load<uint32_t>(data + 4) != kMw2DefChecksum) {
      if (!warned) {
        warned = true;
        XELOGW("Bridge: MW2 offline stats (controller {}) have def {} / {:08X}, "
               "expected {} / {:08X}; unlock skipped",
               c, xe::load<uint32_t>(data), xe::load<uint32_t>(data + 4),
               kMw2DefVersion, kMw2DefChecksum);
      }
      continue;
    }
    bool changed = false;
    if (xe::load<int32_t>(data + kMw2ExperienceOff) < kMw2MaxExperience) {
      xe::store<int32_t>(data + kMw2ExperienceOff, kMw2MaxExperience);
      changed = true;
    }
    for (size_t i = 0; i < std::size(kMw2ChallengeState); i++) {
      uint8_t& state = data[kMw2ChallengeStateOff + i];
      if (kMw2ChallengeState[i] && state < kMw2ChallengeState[i]) {
        state = kMw2ChallengeState[i];
        uint8_t* progress = data + kMw2ChallengeProgressOff + 4 * i;
        if (xe::load<uint32_t>(progress) < kMw2ChallengeProgress[i]) {
          xe::store<uint32_t>(progress, kMw2ChallengeProgress[i]);
        }
        changed = true;
      }
    }
    const struct {
      uint32_t off;
      const uint8_t* bits;
      size_t size;
    } bitsets[] = {
        {kMw2IconUnlockedOff, kMw2IconUnlockedBits, sizeof(kMw2IconUnlockedBits)},
        {kMw2TitleUnlockedOff, kMw2TitleUnlockedBits, sizeof(kMw2TitleUnlockedBits)},
        {kMw2KillstreakUnlockedOff, kMw2KillstreakUnlockedBits,
         sizeof(kMw2KillstreakUnlockedBits)},
    };
    for (const auto& set : bitsets) {
      for (size_t k = 0; k < set.size; k++) {
        uint8_t& b = data[set.off + k];
        if ((b | set.bits[k]) != b) {
          b |= set.bits[k];
          changed = true;
        }
      }
    }
    if (changed) {
      src[kWriteNeeded] = 1;
      XELOGI("Bridge: MW2 offline profile (controller {}) raised to max level "
             "with everything unlocked",
             c);
    }
  }
}

// ---- Queue a console command in the guest ---------------------------------
// Calls the title's own Cbuf_AddText(0, text) on a short-lived guest thread.
// IW's Cbuf_AddText takes the command-buffer critical section, so it is safe
// next to the game's main thread, which runs the command on its next frame
// exactly as if it had been typed. `expect` = the function's first words (the
// build check; a different build or the single-player xex is left alone).
bool RunGuestCbuf(uint32_t cbuf_add_text, const uint32_t (&expect)[8],
                  const std::string& text) {
  KernelState* ks = kernel_state();
  if (!ks || text.empty() || text.size() > 1000) {
    return false;
  }
  Memory* memory = ks->memory();
  if (!memory->LookupHeap(cbuf_add_text)) {
    return false;
  }
  auto* words = memory->TranslateVirtual<xe::be<uint32_t>*>(cbuf_add_text);
  for (int i = 0; i < 8; i++) {
    if (expect[i] && uint32_t(words[i]) != expect[i]) {
      XELOGW("Bridge: Cbuf_AddText {:08X} is not the expected code; command "
             "not run",
             cbuf_add_text);
      return false;
    }
  }
  const uint32_t size = uint32_t(text.size() + 1);
  const uint32_t guest_text = memory->SystemHeapAlloc(size);
  if (!guest_text) {
    return false;
  }
  std::memcpy(memory->TranslateVirtual(guest_text), text.c_str(), size);
  object_ref<XHostThread> thread(new XHostThread(
      ks, 64 * 1024, 0, [ks, cbuf_add_text, guest_text]() {
        uint64_t args[] = {0, guest_text};
        ks->processor()->Execute(XThread::GetCurrentThread()->thread_state(),
                                 cbuf_add_text, args, 2);
        ks->memory()->SystemHeapFree(guest_text);
        return 0;
      }));
  thread->set_name("Crossplay command");
  thread->set_can_debugger_suspend(true);  // runs guest code
  if (XFAILED(thread->Create())) {
    memory->SystemHeapFree(guest_text);
    return false;
  }
  return true;  // the thread owns itself until it exits
}

// ---- MW2: match-end rejoin -------------------------------------------------
// TU9 (resolved 2026-09-25 from the Alpha 482 rosetta):
//   Cbuf_AddText 0x82224990 (Sys_EnterCriticalSection(28) / cmd_textArray
//     0x83174310); anchored by "exec default_mp.cfg" (Com_ExecStartupConfigs).
//   clientUIActives[0].connectionState 0x8267FC48 (stride 3216; read by
//     CL_DisconnectPacket 0x821441E8): 0 = CA_DISCONNECTED ... 9 = CA_ACTIVE.
// A host's match end sends "disconnect EXE_MATCHENDED"; CL_DisconnectError
// (0x82144058) makes that a quiet ERR_DISCONNECT, so the player lands in the
// menus with state 0 and the command the System Link browser would run
// ("connect <xnaddr> <xnkey> <xnkid> <pslots> <prslots> <nonce>",
// LAN_GetServerAddressString) brings them back.
constexpr uint32_t kMw2CbufAddText = 0x82224990;
constexpr uint32_t kMw2CbufExpect[8] = {
    0x7D8802A6,  // mflr r12
    0x9181FFF8,  // stw r12, -8(r1)
    0xFBC1FFE8,  // std r30, -0x18(r1)
    0xFBE1FFF0,  // std r31, -0x10(r1)
    0x9421FF90,  // stwu r1, -0x70(r1)
    0x7C7E1B78,  // mr r30, r3
    0x3860001C,  // li r3, 28 (CRITSECT_CBUF)
    0x7C9F2378,  // mr r31, r4
};
constexpr uint32_t kMw2ConnectionState = 0x8267FC48;

// ---- MW2 fake Live: one set of stats for System Link and Xbox LIVE ----
// Signed in to "Live", MW2 reads the ONLINE stats source (party lobby class
// menu, Xbox LIVE Create a Class, the stats a party member uploads to the
// host). Its own download of them fails without Live -- "not found" leaves
// them zeroed: level 1, blank custom classes, Create a Class locked. So the
// online source mirrors the offline (System Link) profile, which the unlock
// above keeps at max level, and edits flow both ways:
//   offline changed (System Link Create a Class, unlock) -> online
//   online changed (Xbox LIVE Create a Class)            -> offline, saved
// Only outside a match (no server connection; the party lobby has none), and
// an online block is taken back only when it is a real profile -- same def
// version/format and the same experience as the offline one -- so a zeroed
// or re-downloaded block can never wipe the System Link save.
// controllerStatData 0x831A05C0: 0x6230 per controller, 0x2004 per source
// (0 online, 1 offline): u32 checksum + 8188 bytes (little-endian structured
// data) = 0x2000, then statsFetched +0x2000, statWriteNeeded +0x2001.
void Mw2SyncFakeLiveStats() {
  constexpr uint32_t kStatData = 0x831A05C0;
  constexpr uint32_t kControllerStride = 0x6230;
  constexpr uint32_t kSourceStride = 0x2004;
  constexpr uint32_t kBlock = 0x2000;  // checksum + data, no flags
  constexpr uint32_t kFetched = 0x2000;
  constexpr uint32_t kWriteNeeded = 0x2001;
  static std::vector<uint8_t> synced[4];

  Memory* memory = kernel_state()->memory();
  if (!memory->LookupHeap(kStatData) || !Mw2ClientDisconnected()) {
    return;  // not this build, or in a match
  }
  for (uint32_t c = 0; c < 4; c++) {
    uint8_t* online =
        memory->TranslateVirtual<uint8_t*>(kStatData + c * kControllerStride);
    uint8_t* offline = online + kSourceStride;
    if (!offline[kFetched]) {
      continue;
    }
    const uint8_t* off_data = offline + 4;
    const uint8_t* on_data = online + 4;
    if (xe::load<uint32_t>(off_data) != kMw2DefVersion ||
        xe::load<uint32_t>(off_data + 4) != kMw2DefChecksum) {
      continue;  // offline profile not in the format we know
    }
    std::vector<uint8_t>& snap = synced[c];
    const bool online_real =
        online[kFetched] && xe::load<uint32_t>(on_data) == kMw2DefVersion &&
        xe::load<uint32_t>(on_data + 4) == kMw2DefChecksum &&
        xe::load<int32_t>(on_data + kMw2ExperienceOff) ==
            xe::load<int32_t>(off_data + kMw2ExperienceOff);
    if (!snap.empty() && online_real &&
        std::memcmp(online, snap.data(), kBlock) != 0) {
      std::memcpy(offline, online, kBlock);   // Xbox LIVE edit -> System Link
      offline[kWriteNeeded] = 1;              // saved by the game
      snap.assign(online, online + kBlock);
      XELOGI("Bridge: MW2 classes edited in the Xbox LIVE menus saved to the "
             "System Link profile (controller {})",
             c);
    } else if (snap.empty() || !online[kFetched] ||
               std::memcmp(offline, snap.data(), kBlock) != 0 ||
               std::memcmp(online, offline, kBlock) != 0) {
      std::memcpy(online, offline, kBlock);   // System Link -> Xbox LIVE
      online[kFetched] = 1;
      snap.assign(offline, offline + kBlock);
      XELOGI("Bridge: MW2 Xbox LIVE stats set from the System Link profile "
             "(controller {})",
             c);
    }
  }
}

bool Mw2ClientDisconnected() {
  Memory* memory = kernel_state()->memory();
  if (!memory->LookupHeap(kMw2ConnectionState)) {
    return false;
  }
  return uint32_t(*memory->TranslateVirtual<xe::be<uint32_t>*>(
             kMw2ConnectionState)) == 0;
}

bool Mw2RunCommand(const std::string& text) {
  return RunGuestCbuf(kMw2CbufAddText, kMw2CbufExpect, text);
}

// ---- MW2: join the host's Live private-match lobby (bridge_fake_live) ----
// RE 2026-09-25 (TU9, Alpha 482 names). An accepted invite is kept in
// g_launchData (0x838BCB08); Live_CheckInvites (0x822CBEE0, every Live_Frame)
// runs Live_ConfirmAcceptInvitation while inviteState == 2 (MPACCEPTED) for
// the local user whose cached sign-in state is Live and whose XUID low dword
// equals invitedXUID -> MPAcceptInvite -> PartyHost_TestPotentialHost ->
// Party_AskToJoinParty, i.e. the game's own join. Writing those fields is the
// guide's "accept invite" without the Live round trip:
//   invitedXUID +0x390 (0x838BCE98), hostInfo XSESSION_INFO +0x398
//   (0x838BCEA0, 60 bytes), fromGameInvite +0x3D4 (0x838BCEDC; becomes the
//   memberJoin `invited` bit -- a private match has no public slots, so the
//   host refuses without it), and LAST inviteState +0x38C (0x838BCE94) = 2.
// Accept gates that Xenia can't satisfy without Live are met in memory:
//   have-playlists byte 0x83180398 and the playlist version 0x836B9740 (must
//   equal the host's -- joinParty is refused otherwise; private match runs
//   no playlist rules); online stats (source 0, controllerStatData
//   0x831A05C0, fetched +0x2000) copied from the offline profile, which
//   Mw2OnTick keeps unlocked -- an in-party player's classes come from the
//   ONLINE source; NAT type 0x8253A508 = 1 (open; default 3 strict).
// xblive_playEvenIfDown (dvar* 0x83176ED8, value +8) = 1 makes the Live
// failures on this path (session create, LSP enumeration) non-fatal.
// In party: g_lobbyData (0x8268EE78) inParty +0x14E8.
constexpr uint32_t kMw2LaunchInviteXuid = 0x838BCE98;
constexpr uint32_t kMw2LaunchHostInfo = 0x838BCEA0;
constexpr uint32_t kMw2LaunchFromGameInvite = 0x838BCEDC;
constexpr uint32_t kMw2LaunchInviteState = 0x838BCE94;
constexpr uint32_t kMw2UserData = 0x838BA820;  // xenonUserData[4], 0x68 each
constexpr uint32_t kMw2UserXuid = 0x28;
constexpr uint32_t kMw2HavePlaylists = 0x83180398;
constexpr uint32_t kMw2PlaylistVersion = 0x836B9740;
constexpr uint32_t kMw2NatType = 0x8253A508;
constexpr uint32_t kMw2PlayEvenIfDown = 0x83176ED8;
constexpr uint32_t kMw2LobbyInParty = 0x8268EE78 + 0x14E8;
constexpr uint32_t kMw2LobbyAreWeHost = 0x8268EE78 + 0x14DC;

// A member of SOMEONE ELSE's lobby party (the host's). Signed in to "Live",
// MW2 hosts a party of its own at the Xbox LIVE menu (inParty 1, areWeHost 1)
// -- that is not being in the host's party.
bool Mw2InParty() {
  Memory* memory = kernel_state()->memory();
  if (!memory->LookupHeap(kMw2LobbyInParty)) {
    return false;
  }
  auto u32 = [memory](uint32_t a) {
    return uint32_t(*memory->TranslateVirtual<xe::be<uint32_t>*>(a));
  };
  return u32(kMw2LobbyInParty) != 0 && u32(kMw2LobbyAreWeHost) == 0;
}

// Ordinary System Link connect (not the host's party member): onlinegame 0.
// "Member" = the host's party lists this player's gamertag -- the rule the
// host plugin uses too (DLL crossplay 12), so both sides agree on whether
// the gamestate carries the constant configstrings. The guest's own
// inParty can be stale (e.g. after the relay gave it a new address), and
// with onlinegame && inParty it would load them while the host sends none.
// With fake Live, onlinegame stays 1 after the Xbox LIVE menus or an earlier
// party session, and a client with onlinegame && inParty loads the constant
// configstrings (CL_ParseGamestate 0x8214AFF8) -- the host sends a System
// Link joiner none, so the join ends in "Our level fastfile is different"
// (console 2026-09-26, mid-match join from the System Link list). The
// command runs on the next frame, well before the gamestate arrives.
void Mw2PrepareConnect(const std::string& host_party_members) {
  if (!XLiveAPI::BridgeFakeLive()) {
    return;
  }
  Memory* memory = kernel_state()->memory();
  constexpr uint32_t kGamertag = 0x838BA820 + 4;  // xenonUserData[0]
  std::string me;
  if (memory->LookupHeap(kGamertag)) {
    const char* g = memory->TranslateVirtual<const char*>(kGamertag);
    me.assign(g, strnlen(g, 16));
  }
  bool listed = false;
  size_t pos = 0;
  while (!me.empty() && pos <= host_party_members.size()) {
    size_t comma = host_party_members.find(',', pos);
    if (comma == std::string::npos) {
      comma = host_party_members.size();
    }
    const std::string name = host_party_members.substr(pos, comma - pos);
    if (name.size() == me.size() &&
        std::equal(name.begin(), name.end(), me.begin(), [](char a, char b) {
          return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b));
        })) {
      listed = true;
      break;
    }
    pos = comma + 1;
  }
  if (listed && Mw2InParty()) {
    XELOGI("Bridge: connect as the host's party member ({})", me);
    return;
  }
  constexpr uint32_t kOnlinegame = 0x825E21C4;  // dvar_t*, value +8
  if (!memory->LookupHeap(kOnlinegame)) {
    return;
  }
  const uint32_t dvar =
      uint32_t(*memory->TranslateVirtual<xe::be<uint32_t>*>(kOnlinegame));
  if (dvar && memory->LookupHeap(dvar) &&
      *memory->TranslateVirtual<uint8_t*>(dvar + 8)) {
    Mw2RunCommand("set onlinegame 0\n");
    XELOGI("Bridge: System Link join: onlinegame 0");
  }
}

bool Mw2JoinParty(const PartyInvite& invite) {
  Memory* memory = kernel_state()->memory();
  // Same executable check as the command hook: never write into another
  // build's memory.
  if (!memory->LookupHeap(kMw2CbufAddText) ||
      uint32_t(*memory->TranslateVirtual<xe::be<uint32_t>*>(
          kMw2CbufAddText + 24)) != kMw2CbufExpect[6]) {
    return false;
  }
  auto u32 = [memory](uint32_t a) {
    return memory->TranslateVirtual<xe::be<uint32_t>*>(a);
  };
  auto u8 = [memory](uint32_t a) { return memory->TranslateVirtual<uint8_t*>(a); };

  const uint64_t xuid = uint64_t(*memory->TranslateVirtual<xe::be<uint64_t>*>(
      kMw2UserData + kMw2UserXuid));
  if (!xuid) {
    XELOGW("Bridge: MW2 has no signed-in XUID yet; lobby join postponed");
    return false;
  }

  // xblive_playEvenIfDown 1 (through the console, so the dvar system knows).
  const uint32_t dvar = *u32(kMw2PlayEvenIfDown);
  if (dvar && memory->LookupHeap(dvar) && !*u8(dvar + 8)) {
    Mw2RunCommand("set xblive_playEvenIfDown 1\n");
  }

  *u32(kMw2NatType) = 1;
  *u8(kMw2HavePlaylists) = 1;
  *u32(kMw2PlaylistVersion) = invite.playlist_version;

  // Online stats = the System Link profile (see Mw2SyncFakeLiveStats).
  Mw2SyncFakeLiveStats();

  *memory->TranslateVirtual<xe::be<uint64_t>*>(kMw2LaunchInviteXuid) = xuid;
  uint8_t* host = u8(kMw2LaunchHostInfo);
  std::memcpy(host, invite.xnkid, 8);
  std::memcpy(host + 8, invite.xnaddr, 36);
  std::memcpy(host + 44, invite.xnkey, 16);
  *u8(kMw2LaunchFromGameInvite) = 1;
  *u32(kMw2LaunchInviteState) = 2;  // last: Live_CheckInvites acts on this
  return true;
}

const TitleHooks kTitles[] = {
    {0x415607E6, "Call of Duty 4", Cod4OnConnect, nullptr, &kCod4AimAssist},
    {0x41560817, "Modern Warfare 2", nullptr, nullptr, &kMw2AimAssist,
     Mw2OnTick, Mw2ClientDisconnected, Mw2RunCommand, Mw2InParty,
     Mw2JoinParty, Mw2PrepareConnect},
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
