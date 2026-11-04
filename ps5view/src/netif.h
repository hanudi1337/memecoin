// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Enumerates the local IPv4 interfaces with their directed broadcast address.
// Sending discovery packets to each subnet broadcast (instead of only
// 255.255.255.255) makes discovery work on PCs with several adapters
// (Hyper-V, VPN, VirtualBox), where Windows would send the limited broadcast
// out of one adapter only.

#ifndef PV_NETIF_H
#define PV_NETIF_H

#include <stddef.h>
#include <stdint.h>

typedef struct pv_netif_t
{
	char name[128];
	char addr[16];      // dotted quad
	char broadcast[16]; // dotted quad
	unsigned prefix_len;
	uint32_t addr_be;      // network byte order
	uint32_t broadcast_be; // network byte order
} PvNetIf;

// Returns the number of entries written (only up, non-loopback IPv4).
size_t pv_netif_list(PvNetIf *out, size_t max);

#endif
