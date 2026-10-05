/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBRIDGE_H_
#define XENIA_KERNEL_XBRIDGE_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include <WS2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

// Crossplay bridge to a real (RGH) Xbox 360 host. The guest keeps talking to
// the 360 host's real address; only the host socket layer knows a relay sits
// in between:
//   send: destination = host (learned or bridge_peer_ip) or 255.255.255.255
//         -> bridge_address, with a 14-byte header in front
//   recv: source = bridge_address -> host:<the receiving socket's own port>
// Everything here is inert unless bridge_enabled is set.
//
// Wire format (must match ClassicsConsole0 pc/tools/xbridge/protocol.py):
//   header  'X' 'B' u8 version(2) u8 flags | u16 guest port | u32 instance
//           token | u32 key tag, all big-endian. flags: bit0 VDP socket,
//           bit1 control message.
//   control messages (v2) go over a separate bridge control socket:
//           up "hello" (proto, title, build, input, aim_assist, class1..5,
//           install, [token]), down "status", "reject", "link", "linked" --
//           see xbridge.cc.
//
// Title-specific behaviour (CoD4: stats loader, host-address parser) is in
// xbridge_titles.cc, keyed by title ID; the rest is title-independent.
namespace xe {
namespace kernel {
namespace bridge {

constexpr size_t kHeaderSize = 14;
constexpr uint8_t kProtocol = 2;

// ---------------------------------------------------------------------------
// CROSSPLAY BUILD NUMBER -- BUMP IT WITH EVERY CHANGE THAT SHIPS.
// ---------------------------------------------------------------------------
// One integer, only ever increasing, sent in every hello. The relay refuses
// builds below its MIN_XENIA_BUILD with "Crossplay: update needed". Any
// change to the crossplay bridge (xbridge*, the bridge edits in xsocket.cc /
// xsession.cc / xam_net.cc, their cvars) that goes to players bumps this by
// one and gets a line in ClassicsConsole0 docs/crossplay/VERSIONING.md.
constexpr uint32_t kCrossplayBuild = 8;  // 8: Discord link prompt + XUID

// Oldest relay (xbridge/versions.py RELAY_BUILD) this build is known to work
// with. An older relay still gets to try, but the player is told the host's
// relay is out of date. Raise it when this build needs something new from
// the relay.
constexpr uint32_t kMinRelayBuild = 4;  // 4: reads hello input / aim_assist
constexpr uint8_t kFlagVdp = 1;
constexpr uint8_t kFlagControl = 2;

// Outgoing UDP datagram from the guest. Redirects `to` to the relay when it
// is bound for the 360 host (or the LAN broadcast) and runs the title's
// send hooks. Returns true when the datagram now goes to the relay and needs
// MakeHeader() in front of it.
bool OnGuestSend(sockaddr* to, const uint8_t* data, size_t len);

// Called first for every outgoing guest UDP datagram. True = the bridge takes
// this datagram over and it must NOT be sent (the socket reports it sent).
// Used for Join Game on a host that runs a Live party: the System Link
// connect never reaches the host, so no stray server client is created that
// would later collide with the player's party slot (see xbridge.cc).
bool ConsumeGuestSend(const uint8_t* data, size_t len);

// Title launch (Emulator, right after XLiveAPI::Init): start the control
// channel so the relay pre-flight -- "relay connected, open System Link" or
// "no answer from relay" -- shows before the player goes looking for a match.
// No-op unless bridge_enabled.
void StartControl();

// ---- Discord link (crossplay build 8, relay build 13+) ----
// Ranked identity = the player's Discord account. Xenia start-up
// (EmulatorWindow::OnEmulatorInitialized) starts the control channel at once,
// without a title, and -- while this install holds no link token -- shows a
// prompt that stays on screen until the relay pushes one: "type /link CODE in
// Discord". The hello carries install=<bridge_install_id> (generated once) and
// token=<bridge_token>; the relay answers with control verbs `link` (code +
// text, or blocked=1 + text) and `linked` (token, xuid), see xbridge.cc. No-op
// unless bridge_enabled.
void StartAtLaunch();

// The XUID the player's Discord link assigned (bridge_link_xuid), 0 = none.
// While bridge_fake_live is on, the profile's online / logon XUID is this
// (UserProfile::GetOnlineXUID), so every XUID-keyed ranked table follows the
// Discord account rather than whatever profile is signed in. Takes effect when
// the title signs in: linking with a game already running needs a restart.
uint64_t LinkedXuid();

// connect() on a UDP socket: redirect only (no header path for send()).
bool RedirectSend(sockaddr* to);

void MakeHeader(uint8_t* out, uint16_t guest_port, bool vdp);

// Incoming UDP datagram. A datagram from the relay is presented as the 360
// host answering on `local_port`; host-address learning happens here.
void MapSource(sockaddr* from, uint16_t local_port, const uint8_t* data,
               size_t len);

// A user module finished loading (KernelState::FinishLoadingUserModule),
// after patch files and before precompilation. In crossplay KBM mode this is
// where the title's aim assist is removed (hid/crossplay_input.h).
void OnUserModuleLoaded(uint32_t title_id, uint32_t entry_point,
                        bool is_title_module);

// ---- title hooks (xbridge_titles.cc) ----

// Crossplay KBM: remove the title's aim assist. The patch is one `blr` over
// the first instruction of the function that fills the aim-assist target
// list (AimAssist_AddToTargetList): the list is rebuilt empty every frame, so
// slowdown, ADS auto-aim, lock-on and melee lunge all find no target -- the
// same end state as the host plugin's KBM mode, which empties the list around
// AimAssist_UpdateGamePadInput. `expect` is the build check: the words that
// must be at `address`, `address + 4`, ... before the patch -- the prologue
// through the function's own load of screenTargetCount, so a different build
// or executable (e.g. the single-player xex) is never touched. 0 = don't care
// (a relative `bl`); expect[0] must be the prologue's `mflr r12`.
struct AimAssistPatch {
  uint32_t address;
  uint32_t expect[8];
};

// The host's Live private-match lobby, as the relay reports it (status
// party_* fields, relay build 5+): the XSESSION_INFO an Xbox invite to that
// lobby carries, plus the playlist version a joiner must match.
struct PartyInvite {
  uint8_t xnkid[8];
  uint8_t xnaddr[36];  // host XNADDR, as stored in guest memory
  uint8_t xnkey[16];
  uint32_t playlist_version;
};

struct TitleHooks {
  uint32_t title_id;
  const char* name;
  // The guest is sending its OOB "connect" to the host through the bridge:
  // the last moment to prepare anything the join reads (CoD4: stats).
  void (*on_connect)();
  // The host's own address (network order) from a relay-sourced datagram,
  // or 0. Null = the generic IW infoResponse "\xnaddr\" parser.
  uint32_t (*host_address)(const uint8_t* data, size_t len);
  // Null = keyboard and mouse can't be used in this title.
  const AimAssistPatch* aim_assist;
  // Called every few seconds from the control-channel thread while the
  // bridge is enabled and this title runs (MW2: offline-profile unlock).
  void (*on_tick)();
  // Match-end rejoin (relay status rejoin=<n>, relay build 5+). When the
  // host's match ends its server shuts down and every remote is dropped;
  // the host's party members are sent to the next match by the party, a
  // bridged player is not. Both null = no automatic rejoin in this title.
  //   client_disconnected: the guest has no server connection (menus).
  //   run_command: queue a console command in the guest; false if this
  //   executable isn't the build the hook was written for.
  bool (*client_disconnected)();
  bool (*run_command)(const std::string& text);
  // Party join (bridge_fake_live): the guest is in the host's lobby party /
  // make the guest accept an invite to it (the title's own accept path).
  // Both null = no party join in this title.
  bool (*in_party)();
  bool (*join_party)(const PartyInvite& invite);
  // The guest is starting a connect to the host (OOB getchallenge): make its
  // state match an ordinary System Link joiner unless it is a member of the
  // host's party (MW2: onlinegame 0, or it loads the constant configstrings
  // and the host's System Link gamestate is refused). Null = nothing to do.
  // host_party_members: the host's lobby party gamertags from the relay
  // (status party_members), "" when unknown.
  void (*prepare_connect)(const std::string& host_party_members);
  // The game is in System Link mode (its server list, where Join Game is).
  // A connect started anywhere else -- the party's "go" connect runs with
  // System Link off -- is never taken for a Join Game. Null = always.
  bool (*system_link_mode)();
};

const TitleHooks* FindTitle(uint32_t title_id);

// Queues a console command in the running title (its own Cbuf_AddText, on a
// short-lived guest thread). False when the title has no command hook or the
// running executable is not the build it was written for. Used by the
// crossplay client settings (hid/crossplay_client.cc).
bool RunTitleCommand(const std::string& text);
uint32_t ParseIwInfoResponseXnaddr(const uint8_t* data, size_t len);

}  // namespace bridge
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBRIDGE_H_
