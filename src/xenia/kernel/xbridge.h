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
//           up "hello" (proto, title, build, class1..5), down "status" or
//           "reject" -- see xbridge.cc.
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
constexpr uint32_t kCrossplayBuild = 1;

// Oldest relay (xbridge/versions.py RELAY_BUILD) this build is known to work
// with. An older relay still gets to try, but the player is told the host's
// relay is out of date. Raise it when this build needs something new from
// the relay.
constexpr uint32_t kMinRelayBuild = 1;
constexpr uint8_t kFlagVdp = 1;
constexpr uint8_t kFlagControl = 2;

// Outgoing UDP datagram from the guest. Redirects `to` to the relay when it
// is bound for the 360 host (or the LAN broadcast) and runs the title's
// send hooks. Returns true when the datagram now goes to the relay and needs
// MakeHeader() in front of it.
bool OnGuestSend(sockaddr* to, const uint8_t* data, size_t len);

// Title launch (Emulator, right after XLiveAPI::Init): start the control
// channel so the relay pre-flight -- "relay connected, open System Link" or
// "no answer from relay" -- shows before the player goes looking for a match.
// No-op unless bridge_enabled.
void StartControl();

// connect() on a UDP socket: redirect only (no header path for send()).
bool RedirectSend(sockaddr* to);

void MakeHeader(uint8_t* out, uint16_t guest_port, bool vdp);

// Incoming UDP datagram. A datagram from the relay is presented as the 360
// host answering on `local_port`; host-address learning happens here.
void MapSource(sockaddr* from, uint16_t local_port, const uint8_t* data,
               size_t len);

// ---- title hooks (xbridge_titles.cc) ----
struct TitleHooks {
  uint32_t title_id;
  const char* name;
  // The guest is sending its OOB "connect" to the host through the bridge:
  // the last moment to prepare anything the join reads (CoD4: stats).
  void (*on_connect)();
  // The host's own address (network order) from a relay-sourced datagram,
  // or 0. Null = the generic IW infoResponse "\xnaddr\" parser.
  uint32_t (*host_address)(const uint8_t* data, size_t len);
};

const TitleHooks* FindTitle(uint32_t title_id);
uint32_t ParseIwInfoResponseXnaddr(const uint8_t* data, size_t len);

}  // namespace bridge
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBRIDGE_H_
