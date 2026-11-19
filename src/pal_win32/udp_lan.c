/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>

#include "base/log.h"
#include "pal/udp_lan.h"

struct pal_udp_lan {
    SOCKET socket;
    bool bound;
};

static uint32_t local_ip, peer_ip, discovery_group;

static bool parse_ipv4(const char *text, uint32_t *address) {
    IN_ADDR parsed;
    if (InetPtonA(AF_INET, text, &parsed) != 1)
        return false;
    *address = ntohl(parsed.s_addr);
    return true;
}

static bool private_ipv4(const char *text, uint32_t *address) {
    if (!parse_ipv4(text, address))
        return false;
    uint32_t ip = *address;
    return (ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 20) == 0xac1 ||
           (ip >> 16) == 0xc0a8;
}

bool pal_udp_lan_init(void) {
    const char *local = getenv("PS5C_LAN_IPV4");
    if (!local || !*local)
        return false;
    const char *peer = getenv("PS5C_LAN_PEER");
    if (!peer || !*peer)
        peer = local;
    const char *group = getenv("PS5C_LAN_GROUP");
    discovery_group = 0;
    if (group && *group &&
        (!parse_ipv4(group, &discovery_group) || (discovery_group >> 28) != 14)) {
        LOG_W(HLE, "LAN UDP unavailable: PS5C_LAN_GROUP requires a multicast IPv4 address");
        return false;
    }
    WSADATA wsa;
    if (!private_ipv4(local, &local_ip) || !private_ipv4(peer, &peer_ip) ||
        WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOG_W(HLE, "LAN UDP unavailable: configure a loopback or private IPv4 address and peer");
        return false;
    }
    LOG_I(HLE, "LAN UDP enabled on %s, peer %s", local, peer);
    if (discovery_group)
        LOG_I(HLE, "LAN UDP discovery %s is directed to the configured peer", group);
    return true;
}

static int socket_error(void) {
    int error = WSAGetLastError();
    /* Winsock's original socket errors are the BSD numbers plus 10000. */
    return error >= 10001 && error <= 10071 ? 10000 - error : -5;
}

int pal_udp_lan_open(pal_udp_lan **out) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return socket_error();
    BOOL exclusive = TRUE;
    u_long nonblocking = 1;
    if (ioctlsocket(s, FIONBIO, &nonblocking) == SOCKET_ERROR ||
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive,
                   sizeof(exclusive)) == SOCKET_ERROR) {
        int error = socket_error();
        closesocket(s);
        return error;
    }
    pal_udp_lan *udp = malloc(sizeof(*udp));
    if (!udp) {
        closesocket(s);
        return -12;
    }
    *udp = (pal_udp_lan){.socket = s};
    *out = udp;
    return 0;
}

void pal_udp_lan_close(pal_udp_lan *udp) {
    closesocket(udp->socket);
    free(udp);
}

static struct sockaddr_in endpoint(uint32_t address, uint16_t port) {
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port)};
    sa.sin_addr.s_addr = htonl(address);
    return sa;
}

int pal_udp_lan_bind(pal_udp_lan *udp, uint32_t address, uint16_t port) {
    /* A guest with no configured interface may bind to wildcard or loopback. */
    if (address && address != local_ip && address != 0x7f000001)
        return -49;
    struct sockaddr_in sa = endpoint(local_ip, port);
    if (bind(udp->socket, (const struct sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR)
        return socket_error();
    udp->bound = true;
    return 0;
}

int pal_udp_lan_name(pal_udp_lan *udp, uint32_t *address, uint16_t *port) {
    *address = 0;
    *port = 0;
    if (!udp->bound)
        return 0;
    struct sockaddr_in sa;
    int size = sizeof(sa);
    if (getsockname(udp->socket, (struct sockaddr *)&sa, &size) == SOCKET_ERROR)
        return socket_error();
    *address = ntohl(sa.sin_addr.s_addr);
    *port = ntohs(sa.sin_port);
    return 0;
}

int pal_udp_lan_join_group(uint32_t group, uint32_t interface_address) {
    /* The guest receives discovery over the same unicast socket as game data. */
    if (!discovery_group || group != discovery_group)
        return -45;
    return !interface_address || interface_address == local_ip ? 0 : -49;
}

int pal_udp_lan_send(pal_udp_lan *udp, const void *data, int size, uint32_t address, uint16_t port) {
    if (discovery_group && address == discovery_group)
        address = peer_ip;
    if (address != peer_ip && address != local_ip)
        return -13;
    if (!udp->bound) {
        int error = pal_udp_lan_bind(udp, 0, 0);
        if (error)
            return error;
    }
    struct sockaddr_in sa = endpoint(address, port);
    int sent = sendto(udp->socket, data, size, 0, (const struct sockaddr *)&sa, sizeof(sa));
    return sent == SOCKET_ERROR ? socket_error() : sent;
}

int pal_udp_lan_receive(pal_udp_lan *udp, void *data, int size, uint32_t *address, uint16_t *port) {
    if (!udp->bound)
        return -35;
    for (int attempt = 0; attempt < 16; attempt++) {
        struct sockaddr_in sa;
        int length = sizeof(sa);
        int count = recvfrom(udp->socket, data, size, 0, (struct sockaddr *)&sa, &length);
        if (count == SOCKET_ERROR && WSAGetLastError() == WSAECONNRESET)
            continue; /* ICMP port unreachable for an earlier datagram. */
        if (count == SOCKET_ERROR)
            return socket_error();
        uint32_t sender = ntohl(sa.sin_addr.s_addr);
        if (sender == peer_ip || sender == local_ip) {
            *address = sender;
            *port = ntohs(sa.sin_port);
            return count;
        }
    }
    return -35;
}
