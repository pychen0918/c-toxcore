/*
 * LAN discovery implementation.
 */

/*
 * Copyright © 2016-2017 The TokTok team.
 * Copyright © 2013 Tox project.
 *
 * This file is part of Tox, the free peer to peer instant messenger.
 *
 * Tox is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Tox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (c) 2019 ioeXNetwork
 *
 * This file is part of Tox, the free peer to peer instant messenger.
 *
 * Tox is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Tox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "LAN_discovery.h"

#include "util.h"

/* Used for get_broadcast(). */
#ifdef __linux
#include <linux/netdevice.h>
#include <sys/ioctl.h>
#endif

#define MAX_INTERFACES 16


/* TODO: multiple threads might concurrently try to set these, and it isn't clear that this couldn't lead to undesirable
 * behaviour. Consider storing the data in per-instance variables instead. */
static int     broadcast_count = -1;
static IP_Port broadcast_ip_ports[MAX_INTERFACES];

#if defined(_WIN32) || defined(__WIN32__) || defined (WIN32)

#include <iphlpapi.h>

static void fetch_broadcast_info(uint16_t port)
{
    IP_ADAPTER_INFO *pAdapterInfo = (IP_ADAPTER_INFO *)malloc(sizeof(IP_ADAPTER_INFO));
    unsigned long ulOutBufLen = sizeof(IP_ADAPTER_INFO);

    if (pAdapterInfo == NULL) {
        return;
    }

    if (GetAdaptersInfo(pAdapterInfo, &ulOutBufLen) == ERROR_BUFFER_OVERFLOW) {
        free(pAdapterInfo);
        pAdapterInfo = (IP_ADAPTER_INFO *)malloc(ulOutBufLen);

        if (pAdapterInfo == NULL) {
            return;
        }
    }

    /* We copy these to the static variables broadcast_* only at the end of fetch_broadcast_info().
     * The intention is to ensure that even if multiple threads enter fetch_broadcast_info() concurrently, only valid
     * interfaces will be set to be broadcast to.
     * */
    int count = 0;
    IP_Port ip_ports[MAX_INTERFACES];

    int ret;

    if ((ret = GetAdaptersInfo(pAdapterInfo, &ulOutBufLen)) == NO_ERROR) {
        IP_ADAPTER_INFO *pAdapter = pAdapterInfo;

        while (pAdapter) {
            IP gateway = {0}, subnet_mask = {0};

            if (addr_parse_ip(pAdapter->IpAddressList.IpMask.String, &subnet_mask)
                    && addr_parse_ip(pAdapter->GatewayList.IpAddress.String, &gateway)) {
                if (gateway.family == AF_INET && subnet_mask.family == AF_INET) {
                    IP_Port *ip_port = &ip_ports[count];
                    ip_port->ip.family = AF_INET;
                    uint32_t gateway_ip = net_ntohl(gateway.ip4.uint32), subnet_ip = net_ntohl(subnet_mask.ip4.uint32);
                    uint32_t broadcast_ip = gateway_ip + ~subnet_ip - 1;
                    ip_port->ip.ip4.uint32 = net_htonl(broadcast_ip);
                    ip_port->port = port;
                    count++;

                    if (count >= MAX_INTERFACES) {
                        break;
                    }
                }
            }

            pAdapter = pAdapter->Next;
        }
    }

    if (pAdapterInfo) {
        free(pAdapterInfo);
    }

    broadcast_count = count;

    for (uint32_t i = 0; i < count; i++) {
        broadcast_ip_ports[i] = ip_ports[i];
    }
}

#elif defined(__linux__)

static void fetch_broadcast_info(uint16_t port)
{
    /* Not sure how many platforms this will run on,
     * so it's wrapped in __linux for now.
     * Definitely won't work like this on Windows...
     */
    broadcast_count = 0;
    Socket sock = 0;

    if ((sock = net_socket(TOX_AF_INET, TOX_SOCK_STREAM, 0)) < 0) {
        return;
    }

    /* Configure ifconf for the ioctl call. */
    struct ifreq i_faces[MAX_INTERFACES];
    memset(i_faces, 0, sizeof(struct ifreq) * MAX_INTERFACES);

    struct ifconf ifconf;
    ifconf.ifc_buf = (char *)i_faces;
    ifconf.ifc_len = sizeof(i_faces);

    if (ioctl(sock, SIOCGIFCONF, &ifconf) < 0) {
        close(sock);
        return;
    }

    /* We copy these to the static variables broadcast_* only at the end of fetch_broadcast_info().
     * The intention is to ensure that even if multiple threads enter fetch_broadcast_info() concurrently, only valid
     * interfaces will be set to be broadcast to.
     * */
    int count = 0;
    IP_Port ip_ports[MAX_INTERFACES];

    /* ifconf.ifc_len is set by the ioctl() to the actual length used;
     * on usage of the complete array the call should be repeated with
     * a larger array, not done (640kB and 16 interfaces shall be
     * enough, for everybody!)
     */
    int i, n = ifconf.ifc_len / sizeof(struct ifreq);

    for (i = 0; i < n; i++) {
        /* there are interfaces with are incapable of broadcast */
        if (ioctl(sock, SIOCGIFBRDADDR, &i_faces[i]) < 0) {
            continue;
        }

        /* moot check: only AF_INET returned (backwards compat.) */
        if (i_faces[i].ifr_broadaddr.sa_family != AF_INET) {
            continue;
        }

        struct sockaddr_in *sock4 = (struct sockaddr_in *)&i_faces[i].ifr_broadaddr;

        if (count >= MAX_INTERFACES) {
            break;
        }

        IP_Port *ip_port = &ip_ports[count];
        ip_port->ip.family = AF_INET;
        ip_port->ip.ip4.uint32 = sock4->sin_addr.s_addr;

        if (ip_port->ip.ip4.uint32 == 0) {
            continue;
        }

        ip_port->port = port;
        count++;
    }

    close(sock);

    broadcast_count = count;

    for (uint32_t i = 0; i < count; i++) {
        broadcast_ip_ports[i] = ip_ports[i];
    }
}

#else // TODO(irungentoo): Other platforms?

static void fetch_broadcast_info(uint16_t port)
{
    broadcast_count = 0;
}

#endif
/* Send packet to all IPv4 broadcast addresses
 *
 *  return 1 if sent to at least one broadcast target.
 *  return 0 on failure to find any valid broadcast target.
 */
static uint32_t send_broadcasts(Networking_Core *net, uint16_t port, const uint8_t *data, uint16_t length)
{
    /* fetch only once? on every packet? every X seconds?
     * old: every packet, new: once */
    if (broadcast_count < 0) {
        fetch_broadcast_info(port);
    }

    if (!broadcast_count) {
        return 0;
    }

    int i;

    for (i = 0; i < broadcast_count; i++) {
        sendpacket(net, broadcast_ip_ports[i], data, length);
    }

    return 1;
}

/* Return the broadcast ip. */
static IP broadcast_ip(Family family_socket, Family family_broadcast)
{
    IP ip;
    ip_reset(&ip);

    if (family_socket == AF_INET6) {
        if (family_broadcast == AF_INET6) {
            ip.family = AF_INET6;
            /* FF02::1 is - according to RFC 4291 - multicast all-nodes link-local */
            /* FE80::*: MUST be exact, for that we would need to look over all
             * interfaces and check in which status they are */
            ip.ip6.uint8[ 0] = 0xFF;
            ip.ip6.uint8[ 1] = 0x02;
            ip.ip6.uint8[15] = 0x01;
        } else if (family_broadcast == AF_INET) {
            ip.family = AF_INET6;
            ip.ip6.uint32[0] = 0;
            ip.ip6.uint32[1] = 0;
            ip.ip6.uint32[2] = net_htonl(0xFFFF);
            ip.ip6.uint32[3] = INADDR_BROADCAST;
        }
    } else if (family_socket == AF_INET) {
        if (family_broadcast == AF_INET) {
            ip.family = AF_INET;
            ip.ip4.uint32 = INADDR_BROADCAST;
        }
    }

    return ip;
}

/* Is IP a local ip or not. */
bool Local_ip(IP ip)
{
    if (ip.family == AF_INET) {
        IP4 ip4 = ip.ip4;

        /* Loopback. */
        if (ip4.uint8[0] == 127) {
            return 1;
        }
    } else {
        /* embedded IPv4-in-IPv6 */
        if (IPV6_IPV4_IN_V6(ip.ip6)) {
            IP ip4;
            ip4.family = AF_INET;
            ip4.ip4.uint32 = ip.ip6.uint32[3];
            return Local_ip(ip4);
        }

        /* localhost in IPv6 (::1) */
        if (ip.ip6.uint64[0] == 0 && ip.ip6.uint32[2] == 0 && ip.ip6.uint32[3] == net_htonl(1)) {
            return 1;
        }
    }

    return 0;
}

/*  return 0 if ip is a LAN ip.
 *  return -1 if it is not.
 */
int LAN_ip(IP ip)
{
    if (Local_ip(ip)) {
        return 0;
    }

    if (ip.family == AF_INET) {
        IP4 ip4 = ip.ip4;

        /* 10.0.0.0 to 10.255.255.255 range. */
        if (ip4.uint8[0] == 10) {
            return 0;
        }

        /* 172.16.0.0 to 172.31.255.255 range. */
        if (ip4.uint8[0] == 172 && ip4.uint8[1] >= 16 && ip4.uint8[1] <= 31) {
            return 0;
        }

        /* 192.168.0.0 to 192.168.255.255 range. */
        if (ip4.uint8[0] == 192 && ip4.uint8[1] == 168) {
            return 0;
        }

        /* 169.254.1.0 to 169.254.254.255 range. */
        if (ip4.uint8[0] == 169 && ip4.uint8[1] == 254 && ip4.uint8[2] != 0
                && ip4.uint8[2] != 255) {
            return 0;
        }

        /* RFC 6598: 100.64.0.0 to 100.127.255.255 (100.64.0.0/10)
         * (shared address space to stack another layer of NAT) */
        if ((ip4.uint8[0] == 100) && ((ip4.uint8[1] & 0xC0) == 0x40)) {
            return 0;
        }
    } else if (ip.family == AF_INET6) {

        /* autogenerated for each interface: FE80::* (up to FEBF::*)
           FF02::1 is - according to RFC 4291 - multicast all-nodes link-local */
        if (((ip.ip6.uint8[0] == 0xFF) && (ip.ip6.uint8[1] < 3) && (ip.ip6.uint8[15] == 1)) ||
                ((ip.ip6.uint8[0] == 0xFE) && ((ip.ip6.uint8[1] & 0xC0) == 0x80))) {
            return 0;
        }

        /* embedded IPv4-in-IPv6 */
        if (IPV6_IPV4_IN_V6(ip.ip6)) {
            IP ip4;
            ip4.family = AF_INET;
            ip4.ip4.uint32 = ip.ip6.uint32[3];
            return LAN_ip(ip4);
        }
    }

    return -1;
}

static int handle_LANdiscovery(void *object, IP_Port source, const uint8_t *packet, uint16_t length, void *userdata)
{
    DHT *dht = (DHT *)object;

    if (LAN_ip(source.ip) == -1) {
        return 1;
    }

    if (length != CRYPTO_PUBLIC_KEY_SIZE + 1) {
        return 1;
    }

    DHT_bootstrap(dht, source, packet + 1);
    return 0;
}


int send_LANdiscovery(uint16_t port, DHT *dht)
{
    ELASTOS_VLA(uint8_t, data, CRYPTO_PUBLIC_KEY_SIZE + 1);
    data[0] = NET_PACKET_LAN_DISCOVERY;
    id_copy(data + 1, dht->self_public_key);

    send_broadcasts(dht->net, port, data, 1 + CRYPTO_PUBLIC_KEY_SIZE);

    int res = -1;
    IP_Port ip_port;
    ip_port.port = port;

    /* IPv6 multicast */
    if (dht->net->family == AF_INET6) {
        ip_port.ip = broadcast_ip(AF_INET6, AF_INET6);

        if (ip_isset(&ip_port.ip)) {
            if (sendpacket(dht->net, ip_port, data, 1 + CRYPTO_PUBLIC_KEY_SIZE) > 0) {
                res = 1;
            }
        }
    }

    /* IPv4 broadcast (has to be IPv4-in-IPv6 mapping if socket is AF_INET6 */
    ip_port.ip = broadcast_ip(dht->net->family, AF_INET);

    if (ip_isset(&ip_port.ip)) {
        if (sendpacket(dht->net, ip_port, data, 1 + CRYPTO_PUBLIC_KEY_SIZE)) {
            res = 1;
        }
    }

    return res;
}


void LANdiscovery_init(DHT *dht)
{
    networking_registerhandler(dht->net, NET_PACKET_LAN_DISCOVERY, &handle_LANdiscovery, dht);
}

void LANdiscovery_kill(DHT *dht)
{
    networking_registerhandler(dht->net, NET_PACKET_LAN_DISCOVERY, NULL, NULL);
}
