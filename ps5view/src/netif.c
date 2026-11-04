// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "netif.h"

#include "log.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

static void fill_entry(PvNetIf *e, const char *name, uint32_t addr_be, uint32_t mask_be, unsigned prefix_len)
{
	memset(e, 0, sizeof(*e));
	pv_strlcpy(e->name, name ? name : "?", sizeof(e->name));
	e->addr_be = addr_be;
	e->broadcast_be = addr_be | ~mask_be;
	e->prefix_len = prefix_len;
	struct in_addr a;
	a.s_addr = e->addr_be;
	inet_ntop(AF_INET, &a, e->addr, sizeof(e->addr));
	a.s_addr = e->broadcast_be;
	inet_ntop(AF_INET, &a, e->broadcast, sizeof(e->broadcast));
}

#ifdef _WIN32

size_t pv_netif_list(PvNetIf *out, size_t max)
{
	ULONG size = 16 * 1024;
	IP_ADAPTER_ADDRESSES *addrs = NULL;
	ULONG r = ERROR_BUFFER_OVERFLOW;
	for(int attempt = 0; attempt < 3 && r == ERROR_BUFFER_OVERFLOW; attempt++)
	{
		free(addrs);
		addrs = malloc(size);
		if(!addrs)
			return 0;
		r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
				NULL, addrs, &size);
	}
	if(r != NO_ERROR)
	{
		LOGW("netif", "GetAdaptersAddresses fehlgeschlagen: %lu", (unsigned long)r);
		free(addrs);
		return 0;
	}
	size_t n = 0;
	for(IP_ADAPTER_ADDRESSES *a = addrs; a && n < max; a = a->Next)
	{
		if(a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
			continue;
		char *name = pv_wide_to_utf8(a->FriendlyName);
		for(IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && n < max; u = u->Next)
		{
			if(!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET)
				continue;
			unsigned prefix = u->OnLinkPrefixLength;
			if(prefix > 32)
				continue;
			uint32_t addr_be = ((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr;
			uint32_t mask_host = prefix ? (0xffffffffu << (32 - prefix)) : 0;
			// skip link-local 169.254/16: no console lives there
			if((ntohl(addr_be) & 0xffff0000u) == 0xa9fe0000u)
				continue;
			fill_entry(&out[n++], name, addr_be, htonl(mask_host), prefix);
		}
		free(name);
	}
	free(addrs);
	return n;
}

#else

static unsigned mask_prefix(uint32_t mask_be)
{
	uint32_t m = ntohl(mask_be);
	unsigned p = 0;
	while(m & 0x80000000u)
	{
		p++;
		m <<= 1;
	}
	return p;
}

size_t pv_netif_list(PvNetIf *out, size_t max)
{
	struct ifaddrs *ifs;
	if(getifaddrs(&ifs) != 0)
	{
		LOGW("netif", "getifaddrs fehlgeschlagen");
		return 0;
	}
	size_t n = 0;
	for(struct ifaddrs *i = ifs; i && n < max; i = i->ifa_next)
	{
		if(!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask)
			continue;
		if(!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK))
			continue;
		uint32_t addr_be = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
		uint32_t mask_be = ((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr;
		fill_entry(&out[n++], i->ifa_name, addr_be, mask_be, mask_prefix(mask_be));
	}
	freeifaddrs(ifs);
	return n;
}

#endif
