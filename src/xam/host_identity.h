/**
 * @file        xam/host_identity.h
 *
 * @brief       Per machine identity for System Link: LAN address, ethernet address, XUIDs.
 *
 * Ported from the BO2 MP recompile (bo2-bisect syslink/0004 and its app's XNet layer). Before
 * this, every machine reported loopback, the ethernet address CC:CC:CC:CC:CC:CC and the XUID
 * 0xB13EBABEBABEBABE, so two machines in one System Link session looked like one player
 * rejoining, and a host advertised an address no peer could reach.
 */

#pragma once

#include <rex/types.h>

namespace rex::xam {

/** A stable 64 bit hash of this machine (MachineGuid on Windows, /etc/machine-id elsewhere). */
u64 HostMachineHash();

/**
 * The LAN address this machine uses to reach other hosts, as a host order number
 * (192.168.1.100 is 0xC0A80164). The routing table is asked with a UDP connect, which sends no
 * packet. Loopback when the host has no route.
 */
u32 HostLanAddress();

/** A stable, locally administered unicast ethernet address derived from HostMachineHash. */
void HostEthernetAddress(u8 out_mac[6]);

/** A random 64 bit value for session ids and keys. */
u64 HostRandomU64();

/**
 * The XUID of local user `user_index`: user 0 is the profile XUID, users 1 to 3 are offline
 * XUIDs (0x0009 high bits) that carry 40 bits of the profile XUID, so they differ per machine.
 * Bits 54 and 55 stay clear, since titles reject XUIDs with them set.
 */
u64 LocalUserXuid(u64 profile_xuid, u32 user_index);

}  // namespace rex::xam
