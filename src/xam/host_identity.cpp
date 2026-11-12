/**
 * @file        xam/host_identity.cpp
 *
 * @brief       Per machine identity for System Link. See host_identity.h.
 */

#include "xam/host_identity.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <rex/platform.h>
#include <rex/platform/socket.h>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rex::xam {

namespace {

u64 Fnv1a(const char* data, size_t size) {
  u64 h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size && data[i] != 0; ++i) {
    h = (h ^ static_cast<u8>(data[i])) * 0x100000001B3ull;
  }
  return h;
}

u64 SplitMix64(u64 x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

}  // namespace

u64 HostMachineHash() {
  static const u64 cached = [] {
#if REX_PLATFORM_WIN32
    char guid[64] = {};
    DWORD guid_size = sizeof(guid);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid",
                     RRF_RT_REG_SZ, nullptr, guid, &guid_size) == ERROR_SUCCESS) {
      return Fnv1a(guid, guid_size);
    }
#else
    for (const char* path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
      std::ifstream file(path);
      std::string id;
      if (file && std::getline(file, id) && !id.empty()) {
        return Fnv1a(id.data(), id.size());
      }
    }
    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) == 0 && host[0] != 0) {
      return Fnv1a(host, sizeof(host));
    }
#endif
    // No machine id: the old constant identity, as before this change.
    return u64{0xBABEBABEBABEull};
  }();
  return cached;
}

u32 HostLanAddress() {
  static const u32 cached = [] {
    u32 address = INADDR_LOOPBACK;
    rex::platform::Socket probe = rex::platform::Socket::Create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe.IsValid()) {
      sockaddr_in destination{};
      destination.sin_family = AF_INET;
      destination.sin_port = htons(9);
      destination.sin_addr.s_addr = htonl(0x08080808u);  // never contacted
      if (probe.Connect(reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) ==
          0) {
        sockaddr_in local{};
        i32 length = sizeof(local);
        if (probe.GetLocalName(reinterpret_cast<sockaddr*>(&local), &length) == 0 &&
            local.sin_addr.s_addr != 0) {
          address = ntohl(local.sin_addr.s_addr);
        }
      }
    }
    return address;
  }();
  return cached;
}

void HostEthernetAddress(u8 out_mac[6]) {
  const u64 h = SplitMix64(HostMachineHash());
  out_mac[0] = 0x02;  // locally administered, unicast
  for (int i = 1; i < 6; ++i) {
    out_mac[i] = static_cast<u8>(h >> (8 * i));
  }
}

u64 HostRandomU64() {
  static std::atomic<u64> counter{0};
  const u64 now = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
  const u64 thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
  return SplitMix64(now ^ (thread << 17) ^ HostMachineHash() ^
                    counter.fetch_add(0x9E3779B97F4A7C15ull, std::memory_order_relaxed));
}

u64 LocalUserXuid(u64 profile_xuid, u32 user_index) {
  if (user_index == 0 || user_index >= 4) {
    return profile_xuid;
  }
  return 0x0009000000000000ull | ((profile_xuid & 0x000000FFFFFFFFFFull) << 8) | user_index;
}

}  // namespace rex::xam
