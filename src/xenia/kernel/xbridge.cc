/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xbridge.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/ui/imgui_host_notification.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

#if XE_PLATFORM_WIN32
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET 0x9800000C  // _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

DECLARE_bool(bridge_enabled);
DECLARE_string(bridge_address);
DECLARE_string(bridge_peer_ip);
DECLARE_string(bridge_key);
DECLARE_string(bridge_class_names);

namespace xe {
namespace kernel {
namespace bridge {
namespace {

using namespace std::chrono_literals;

struct Config {
  bool enabled = false;
  sockaddr_in relay = {};
  in_addr peer = {};
  uint32_t key_tag = 0;
};

// FNV-1a of bridge_key. The relay drops datagrams whose header carries a
// different tag, so a relay port open to the internet only feeds the 360's
// engine from Xenia builds configured with the same key. It is a shared
// passphrase, not authentication: the tag rides in clear text.
uint32_t KeyTag(const std::string& key) {
  uint32_t h = 2166136261u;
  for (unsigned char c : key) {
    h = (h ^ c) * 16777619u;
  }
  return h;
}

// The 360 host's own address (its XNADDR ina), learned from the host's
// infoResponse the first time one arrives through the relay -- so players
// never have to configure it, and a console that gets a new LAN IP just
// works. bridge_peer_ip, when set, overrides it. Network byte order; 0 =
// not learned yet (discovery still works: broadcast getinfo is redirected
// regardless).
std::atomic<uint32_t> learned_peer{0};

// bridge_address may be a DNS name (e.g. a dynamic-DNS name for the host's
// home connection). It is resolved when the config is parsed and then again
// every kRelayRefresh on a background thread, so a host whose public IP
// changes mid-session is followed without blocking the game's send path.
// The refreshed address (network order, 0 = none) overrides the parsed one.
std::atomic<uint32_t> resolved_relay{0};
constexpr auto kRelayRefresh = std::chrono::seconds(60);

bool Resolve(const std::string& host, in_addr* out) {
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) {
    return false;
  }
  *out = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
  freeaddrinfo(result);
  return true;
}

void StartRelayRefresh(const std::string& host) {
  static std::mutex mutex;
  static std::string refreshing;
  std::lock_guard<std::mutex> lock(mutex);
  if (refreshing == host) {
    return;
  }
  const bool first = refreshing.empty();
  refreshing = host;
  if (!first) {
    return;  // the running thread reads `refreshing` each round
  }
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(kRelayRefresh);
      std::string name;
      {
        std::lock_guard<std::mutex> lock(mutex);
        name = refreshing;
      }
      in_addr addr = {};
      if (!Resolve(name, &addr)) {
        continue;  // keep the last good address through a DNS hiccup
      }
      const uint32_t old = resolved_relay.exchange(addr.s_addr);
      if (old != addr.s_addr) {
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &addr, ip, sizeof(ip));
        XELOGI("Bridge: relay {} now resolves to {}", name, ip);
      }
    }
  }).detach();
}

Config GetConfig() {
  static std::mutex mutex;
  static Config config;
  static std::string parsed_address;
  static std::string parsed_peer;
  static std::string parsed_key;
  static bool parsed = false;

  std::lock_guard<std::mutex> lock(mutex);
  if (!parsed || parsed_address != cvars::bridge_address ||
      parsed_peer != cvars::bridge_peer_ip || parsed_key != cvars::bridge_key) {
    parsed = true;
    parsed_address = cvars::bridge_address;
    parsed_peer = cvars::bridge_peer_ip;
    parsed_key = cvars::bridge_key;
    config = {};

    const size_t colon = parsed_address.rfind(':');
    const std::string ip = parsed_address.substr(0, colon);
    const int port = colon == std::string::npos
                         ? 0
                         : std::atoi(parsed_address.c_str() + colon + 1);
    bool relay_ok = false;
    resolved_relay.store(0);
    if (inet_pton(AF_INET, ip.c_str(), &config.relay.sin_addr) == 1) {
      relay_ok = true;
    } else if (!ip.empty() && Resolve(ip, &config.relay.sin_addr)) {
      relay_ok = true;
      resolved_relay.store(config.relay.sin_addr.s_addr);
      StartRelayRefresh(ip);
      char resolved[INET_ADDRSTRLEN] = {};
      inet_ntop(AF_INET, &config.relay.sin_addr, resolved, sizeof(resolved));
      XELOGI("Bridge: relay {} resolves to {}", ip, resolved);
    }
    if (port <= 0 || port > 0xFFFF || !relay_ok ||
        (!parsed_peer.empty() &&
         inet_pton(AF_INET, parsed_peer.c_str(), &config.peer) != 1)) {
      if (cvars::bridge_enabled) {
        XELOGE("Bridge: bad bridge_address '{}' or bridge_peer_ip '{}'",
               parsed_address, parsed_peer);
      }
      config = {};
    } else {
      config.relay.sin_family = AF_INET;
      config.relay.sin_port = htons(static_cast<uint16_t>(port));
      config.key_tag = KeyTag(parsed_key);
      XELOGI("Bridge: 360 host {} is reached through relay {}:{}",
             parsed_peer.empty() ? "(learned from its infoResponse)"
                                 : parsed_peer,
             ip, port);
    }
  }
  Config result = config;
  result.enabled = cvars::bridge_enabled && config.relay.sin_port != 0;
  if (const uint32_t refreshed = resolved_relay.load()) {
    result.relay.sin_addr.s_addr = refreshed;
  }
  if (!result.peer.s_addr) {
    result.peer.s_addr = learned_peer.load(std::memory_order_relaxed);
  }
  return result;
}

// The instance token (random per Xenia run) is how the relay tells players
// apart -- behind NAT one Xenia's game sockets arrive from unrelated public
// ports, and the source port can no longer say which socket is VDP.
uint32_t InstanceToken() {
  static const uint32_t token = [] {
    std::random_device rd;
    uint32_t t = 0;
    while (!t) {
      t = rd();
    }
    return t;
  }();
  return token;
}

void WriteHeader(uint8_t* out, uint16_t guest_port, uint8_t flags) {
  const uint32_t token = InstanceToken();
  const uint32_t tag = GetConfig().key_tag;
  out[0] = 'X';
  out[1] = 'B';
  out[2] = kProtocol;
  out[3] = flags;
  out[4] = uint8_t(guest_port >> 8);
  out[5] = uint8_t(guest_port);
  for (int i = 0; i < 4; i++) {
    out[6 + i] = uint8_t(token >> (24 - 8 * i));
    out[10 + i] = uint8_t(tag >> (24 - 8 * i));
  }
}

const TitleHooks* CurrentTitle() {
  return FindTitle(kernel_state() ? kernel_state()->title_id() : 0);
}

bool IsOobConnect(const uint8_t* data, size_t len) {
  // OOB packets on a VDP game socket are framed: [u16 cbGameData] first.
  for (size_t off : {size_t(0), size_t(2)}) {
    if (data && len >= off + 12 &&
        !std::memcmp(data + off, "\xFF\xFF\xFF\xFF" "connect ", 12)) {
      return true;
    }
  }
  return false;
}

// Crossplay notices carry instructions ("open System Link", "check
// bridge_address"), so they stay up longer than Xenia's 4.5 s default.
constexpr uint32_t kNoticeMs = 10000;
constexpr uint32_t kProblemNoticeMs = 15000;  // rejects, relay not answering

void Notify(std::string title, std::string text,
            uint32_t duration_ms = kNoticeMs) {
  Emulator* emulator = kernel_state() ? kernel_state()->emulator() : nullptr;
  if (!emulator || !emulator->display_window() || !emulator->imgui_drawer()) {
    return;
  }
  emulator->display_window()->app_context().CallInUIThread(
      [emulator, title, text, duration_ms]() {
        auto* window = new xe::ui::HostNotificationWindow(
            emulator->imgui_drawer(), title, text, 0);
        window->SetDuration(duration_ms);
      });
}

// ---- control channel ------------------------------------------------------
// A socket of our own (never the guest's), so control traffic never mixes
// with game datagrams in either direction. It says hello to the relay at
// title launch (StartControl), every kHelloEvery after that, and at once when
// the guest connects to a host; the relay answers "status" (host up/down,
// title, players) or "reject" (wrong key, wrong game, lobby full). Changes
// are logged; the ones a player must act on also show as an on-screen
// notification.
//
//   hello   proto, title (hex), build, class1..class5 (bridge_class_names)
//   status  proto, host (up|down), title, players, max, [peer]
//   reject  proto, reason (key|title|full|version), text
class ControlChannel {
 public:
  static ControlChannel& Get() {
    static ControlChannel* channel = new ControlChannel();  // lives forever
    return *channel;
  }

  void EnsureRunning() {
    std::call_once(started_, [this] { std::thread([this] { Run(); }).detach(); });
  }

  void HelloNow() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      hello_now_ = true;
    }
    wake_.notify_one();
  }

 private:
  static constexpr auto kHelloEvery = 5s;
  static constexpr auto kSilentAfter = 15s;

  std::string BuildHello() {
    const uint32_t title_id = kernel_state() ? kernel_state()->title_id() : 0;
    std::string body = "hello\nproto=" + std::to_string(kProtocol);
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%08X", title_id);
    body += "\ntitle=";
    body += hex;
    body += "\nbuild=" + std::to_string(kCrossplayBuild);
    body += "\nbuild_name=xenia-netplay " __DATE__;
    // Class names: trimmed to 20 characters and stripped of quotes,
    // backslashes, semicolons and control characters -- they end up inside
    // a server command on the host.
    const std::string& list = cvars::bridge_class_names;
    size_t start = 0;
    for (int i = 1; i <= 5 && start <= list.size(); i++) {
      size_t comma = list.find(',', start);
      if (comma == std::string::npos) {
        comma = list.size();
      }
      std::string name;
      for (size_t c = start; c < comma && name.size() < 20; c++) {
        const char ch = list[c];
        if (uint8_t(ch) >= 0x20 && ch != '"' && ch != '\\' && ch != ';' &&
            ch != 0x7F) {
          name += ch;
        }
      }
      if (!name.empty()) {
        body += "\nclass" + std::to_string(i) + "=" + name;
      }
      start = comma + 1;
    }
    return body;
  }

  static std::map<std::string, std::string> Parse(std::string_view text,
                                                  std::string* verb) {
    std::map<std::string, std::string> fields;
    size_t pos = 0;
    bool first = true;
    while (pos <= text.size()) {
      size_t nl = text.find('\n', pos);
      if (nl == std::string_view::npos) {
        nl = text.size();
      }
      const std::string_view line = text.substr(pos, nl - pos);
      if (first) {
        *verb = std::string(line);
        first = false;
      } else if (const size_t eq = line.find('='); eq != std::string_view::npos) {
        fields[std::string(line.substr(0, eq))] = std::string(line.substr(eq + 1));
      }
      pos = nl + 1;
    }
    return fields;
  }

  void Handle(const uint8_t* data, int len) {
    if (len < int(kHeaderSize) || data[0] != 'X' || data[1] != 'B' ||
        !(data[3] & kFlagControl)) {
      return;
    }
    const uint32_t token = uint32_t(data[6]) << 24 | uint32_t(data[7]) << 16 |
                           uint32_t(data[8]) << 8 | data[9];
    if (token != InstanceToken()) {
      return;  // not for us (or spoofed without knowing our token)
    }
    std::string verb;
    const auto f = Parse(
        std::string_view(reinterpret_cast<const char*>(data) + kHeaderSize,
                         size_t(len) - kHeaderSize),
        &verb);
    last_reply_ = std::chrono::steady_clock::now();
    auto get = [&f](const char* k) {
      auto it = f.find(k);
      return it == f.end() ? std::string() : it->second;
    };

    if (verb == "reject") {
      const std::string state = "reject:" + get("reason");
      if (state != state_) {
        state_ = state;
        const std::string text = get("text");
        XELOGE("Bridge: the relay refused this player ({}): {}", get("reason"),
               text);
        Notify(get("reason") == "outdated" ? "Crossplay: update needed"
                                           : "Crossplay: can't join",
               text.empty() ? get("reason") : text, kProblemNoticeMs);
      }
      return;
    }
    if (verb != "status") {
      return;
    }

    // Version contract, the other direction: warn once if the host's relay
    // is older than this build expects (a pre-contract relay sends no
    // relay_build at all).
    const uint32_t relay_build =
        uint32_t(std::strtoul(get("relay_build").c_str(), nullptr, 10));
    if (relay_build < kMinRelayBuild && !warned_relay_) {
      warned_relay_ = true;
      XELOGW("Bridge: the host's relay is build {}; this Xenia build ({}) needs "
             "relay build {} or newer",
             relay_build, kCrossplayBuild, kMinRelayBuild);
      Notify("Crossplay: host relay out of date",
             "The host's relay is older than this Xenia build needs. Ask the "
             "host to update the crossplay relay.",
             kProblemNoticeMs);
    }

    const std::string host = get("host");
    const bool host_up = host == "up";
    if (host == "outdated") {
      if (state_ != "outdated") {
        state_ = "outdated";
        XELOGW("Bridge: the host's console plugin is too old for its relay");
        Notify("Crossplay: host plugin out of date",
               "The host's console plugin is older than their relay needs. "
               "Ask the host to update ClassicsRanked.",
               kProblemNoticeMs);
      }
      return;
    }
    const std::string state = host_up ? "up" : "down";
    const std::string summary = "host " + state + ", " + get("players") + "/" +
                                get("max") + " crossplay players" +
                                (f.count("peer") ? ", you are peer " + get("peer") : "");
    if (summary != summary_) {
      summary_ = summary;
      XELOGI("Bridge: relay status: {}", summary);
    }
    if (state != state_) {
      const bool first = state_.empty() || state_ == "silent";
      state_ = state;
      if (host_up) {
        Notify("Crossplay relay connected",
               "The 360 host is online. Open System Link to find the match.");
      } else if (first) {
        Notify("Crossplay relay connected",
               "The relay answered, but the 360 host isn't running a match yet.");
      } else {
        Notify("Crossplay: host offline",
               "The relay lost the 360 host. It reconnects by itself.");
      }
    }
  }

  void Run() {
    const uint64_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == uint64_t(~0ull)) {
      XELOGE("Bridge: could not open the control socket");
      return;
    }
#if XE_PLATFORM_WIN32
    BOOL report_reset = FALSE;
    DWORD bytes_returned = 0;
    WSAIoctl(SOCKET(sock), SIO_UDP_CONNRESET, &report_reset,
             sizeof(report_reset), nullptr, 0, &bytes_returned, nullptr,
             nullptr);
    DWORD timeout_ms = 500;
    setsockopt(SOCKET(sock), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv = {0, 500000};
    setsockopt(int(sock), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    auto first_hello = std::chrono::steady_clock::time_point{};
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, kHelloEvery, [this] { return hello_now_; });
        hello_now_ = false;
      }
      const Config config = GetConfig();
      if (!config.enabled) {
        continue;
      }
      const std::string body = BuildHello();
      std::vector<uint8_t> packet(kHeaderSize + body.size());
      WriteHeader(packet.data(), 0, kFlagControl);
      std::memcpy(packet.data() + kHeaderSize, body.data(), body.size());
      sendto(sock, reinterpret_cast<const char*>(packet.data()),
             int(packet.size()), 0,
             reinterpret_cast<const sockaddr*>(&config.relay),
             sizeof(config.relay));
      const auto now = std::chrono::steady_clock::now();
      if (first_hello == std::chrono::steady_clock::time_point{}) {
        first_hello = now;
      }

      uint8_t buf[1500];
      for (int i = 0; i < 4; i++) {
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        const int n = int(recvfrom(sock, reinterpret_cast<char*>(buf),
                                   sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&from), &from_len));
        if (n <= 0) {
          break;
        }
        if (from.sin_addr.s_addr == config.relay.sin_addr.s_addr &&
            from.sin_port == config.relay.sin_port) {
          Handle(buf, n);
        }
      }

      const auto since = last_reply_ == std::chrono::steady_clock::time_point{}
                             ? now - first_hello
                             : now - last_reply_;
      if (since > kSilentAfter && state_ != "silent") {
        state_ = "silent";
        summary_.clear();
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &config.relay.sin_addr, ip, sizeof(ip));
        const std::string where =
            std::string(ip) + ":" + std::to_string(ntohs(config.relay.sin_port));
        XELOGW(
            "Bridge: no answer from the crossplay relay at {} -- check "
            "bridge_address, bridge_key, the host's port forward, and that the "
            "relay is running",
            where);
        Notify("Crossplay: no answer from relay",
               "Nothing came back from " + where +
                   ". Check bridge_address in the config, and ask the host "
                   "whether the relay is running and up to date.",
               kProblemNoticeMs);
      }
    }
  }

  std::once_flag started_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool hello_now_ = true;  // first hello goes out at once, not after kHelloEvery
  // Control-thread only:
  std::string state_;    // "", "up", "down", "silent", "reject:<reason>"
  std::string summary_;
  bool warned_relay_ = false;
  std::chrono::steady_clock::time_point last_reply_{};
};

bool RedirectToRelay(sockaddr* to) {
  const Config config = GetConfig();
  sockaddr_in* addr_in = reinterpret_cast<sockaddr_in*>(to);
  if (!config.enabled || addr_in->sin_family != AF_INET) {
    return false;
  }
  if ((config.peer.s_addr && addr_in->sin_addr.s_addr == config.peer.s_addr) ||
      addr_in->sin_addr.s_addr == INADDR_BROADCAST) {
    addr_in->sin_addr = config.relay.sin_addr;
    addr_in->sin_port = config.relay.sin_port;
    return true;
  }
  return false;
}

void LearnPeer(const uint8_t* data, size_t len) {
  const TitleHooks* title = CurrentTitle();
  const uint32_t net = title && title->host_address
                           ? title->host_address(data, len)
                           : ParseIwInfoResponseXnaddr(data, len);
  if (!net || learned_peer.exchange(net) == net) {
    return;
  }
  in_addr addr = {};
  addr.s_addr = net;
  char ip[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &addr, ip, sizeof(ip));
  XELOGI("Bridge: 360 host is {} (from its infoResponse){}", ip,
         cvars::bridge_peer_ip.empty() ? ""
                                       : " -- bridge_peer_ip overrides it");
}

}  // namespace

bool RedirectSend(sockaddr* to) { return RedirectToRelay(to); }

void StartControl() {
  if (!GetConfig().enabled) {
    return;
  }
  XELOGI("Bridge: crossplay build {} (protocol {}), expects relay build {}+",
         kCrossplayBuild, kProtocol, kMinRelayBuild);
  ControlChannel& control = ControlChannel::Get();
  control.EnsureRunning();
  control.HelloNow();
}

bool OnGuestSend(sockaddr* to, const uint8_t* data, size_t len) {
  if (!RedirectToRelay(to)) {
    return false;
  }
  ControlChannel& control = ControlChannel::Get();
  control.EnsureRunning();
  if (IsOobConnect(data, len)) {
    if (const TitleHooks* title = CurrentTitle(); title && title->on_connect) {
      title->on_connect();
    }
    control.HelloNow();  // fresh class names for this join
  }
  return true;
}

void MakeHeader(uint8_t* out, uint16_t guest_port, bool vdp) {
  WriteHeader(out, guest_port, vdp ? kFlagVdp : 0);
}

void MapSource(sockaddr* from, uint16_t local_port, const uint8_t* data,
               size_t len) {
  sockaddr_in* addr_in = reinterpret_cast<sockaddr_in*>(from);
  const Config relay_check = GetConfig();
  if (!relay_check.enabled || addr_in->sin_family != AF_INET ||
      addr_in->sin_addr.s_addr != relay_check.relay.sin_addr.s_addr ||
      addr_in->sin_port != relay_check.relay.sin_port) {
    return;
  }
  // Learn first, so even the very first reply is presented as the host.
  if (data && len) {
    LearnPeer(data, len);
  }
  const Config config = GetConfig();
  if (config.peer.s_addr) {
    addr_in->sin_addr = config.peer;
    addr_in->sin_port = htons(local_port);
  }
}

// IW-engine infoResponse (CoD4 retail sub_821FF618) carries the host's
// XNADDR as "\\xnaddr\\" + 9 x "%08x"; the first dword is ina, the address
// titles then connect to (XNetXnAddrToInAddr, LAN mode). Found plain on the
// system link socket, or behind a 2-byte VDP length on the game socket.
uint32_t ParseIwInfoResponseXnaddr(const uint8_t* data, size_t len) {
  static const char kInfo[] = "infoResponse";
  static const char kKey[] = "\\xnaddr\\";
  for (size_t off : {size_t(0), size_t(2)}) {
    if (len < off + 4 + sizeof(kInfo) - 1 ||
        std::memcmp(data + off, "\xFF\xFF\xFF\xFF", 4) ||
        std::memcmp(data + off + 4, kInfo, sizeof(kInfo) - 1)) {
      continue;
    }
    const std::string_view text(reinterpret_cast<const char*>(data) + off,
                                len - off);
    const size_t at = text.find(kKey);
    if (at == std::string_view::npos ||
        text.size() < at + sizeof(kKey) - 1 + 8) {
      return 0;
    }
    uint32_t ina = 0;
    const char* hex = text.data() + at + sizeof(kKey) - 1;
    for (int i = 0; i < 8; i++) {
      const char c = hex[i];
      const int v = c >= '0' && c <= '9'   ? c - '0'
                    : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                           : -1;
      if (v < 0) {
        return 0;
      }
      ina = (ina << 4) | uint32_t(v);
    }
    return htonl(ina);
  }
  return 0;
}

}  // namespace bridge
}  // namespace kernel
}  // namespace xe
