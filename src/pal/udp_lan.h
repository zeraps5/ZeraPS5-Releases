/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_PAL_UDP_LAN_H
#define PS5C_PAL_UDP_LAN_H

#include <stdbool.h>
#include <stdint.h>

/* Windows IPv4 datagrams to an explicitly configured peer. Addresses and
 * ports are in host byte order. Failures return negative guest BSD errno. */
typedef struct pal_udp_lan pal_udp_lan;
bool pal_udp_lan_init(void);
int pal_udp_lan_open(pal_udp_lan **out);
void pal_udp_lan_close(pal_udp_lan *udp);
int pal_udp_lan_bind(pal_udp_lan *udp, uint32_t address, uint16_t port);
int pal_udp_lan_name(pal_udp_lan *udp, uint32_t *address, uint16_t *port);
int pal_udp_lan_join_group(uint32_t group, uint32_t interface_address);
int pal_udp_lan_send(pal_udp_lan *udp, const void *data, int size, uint32_t address, uint16_t port);
int pal_udp_lan_receive(pal_udp_lan *udp, void *data, int size, uint32_t *address, uint16_t *port);

#endif
