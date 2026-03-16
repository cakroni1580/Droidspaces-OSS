/*
 * Droidspaces v5 — High-performance Container Runtime
 *
 * ds_dhcp.c — Embedded single-lease DHCP server for NAT containers.
 *
 * Runs as a joinable thread inside the monitor process. Bound exclusively to
 * the container's veth_host interface via SO_BINDTODEVICE so it never
 * interferes with any DHCP server already running on the host.
 *
 * Serves a single static lease (the deterministic IP from veth_peer_ip()) in
 * response to DHCPDISCOVER and DHCPREQUEST. Handles lease renewals for the
 * full container lifetime.
 *
 * This replaces static RTNETLINK IP assignment on the child side, making IP
 * configuration distro-agnostic: every init system (systemd-networkd, OpenRC
 * + udhcpc, dhcpcd, dhclient) speaks DHCP and will configure eth0 correctly
 * without any rootfs modifications.
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "droidspace.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>

/* ---------------------------------------------------------------------------
 * DHCP wire protocol constants  (RFC 2131 / RFC 2132)
 * ---------------------------------------------------------------------------*/

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define BOOTP_REQUEST 1
#define BOOTP_REPLY 2
#define HTYPE_ETHERNET 1
#define DHCP_MAGIC 0x63825363u

/* Option 53 message types */
#define DHCPDISCOVER 1
#define DHCPOFFER 2
#define DHCPREQUEST 3
#define DHCPACK 5

/* Option codes */
#define OPT_SUBNET_MASK 1
#define OPT_ROUTER 3
#define OPT_DNS 6
#define OPT_LEASE_TIME 51
#define OPT_MSG_TYPE 53
#define OPT_SERVER_ID 54
#define OPT_RENEWAL_T1 58
#define OPT_REBIND_T2 59
#define OPT_END 255
#define OPT_PAD 0

/* Lease timings (seconds) */
#define DHCP_LEASE_SEC 86400u /* 24 h */
#define DHCP_T1_SEC 43200u    /* 12 h */
#define DHCP_T2_SEC 75600u    /* 21 h */

/* ---------------------------------------------------------------------------
 * DHCP packet layout  (RFC 2131 §2 fixed-format fields)
 *
 * Total: 44 + 16 + 64 + 128 + 4 + 308 = 564 bytes ≤ 576 minimum MTU.
 * The options field is large enough for all options we ever send.
 * ---------------------------------------------------------------------------*/

struct dhcp_pkt {
  uint8_t op;    /* 1=REQUEST, 2=REPLY               */
  uint8_t htype; /* 1=Ethernet                        */
  uint8_t hlen;  /* 6                                 */
  uint8_t hops;  /* 0                                 */
  uint32_t xid;  /* transaction id (network order)    */
  uint16_t secs;
  uint16_t flags;     /* bit15=BROADCAST flag              */
  uint32_t ciaddr;    /* client IP (0 if unknown)          */
  uint32_t yiaddr;    /* "your" IP = offered address       */
  uint32_t siaddr;    /* server next-hop IP                */
  uint32_t giaddr;    /* relay agent IP                    */
  uint8_t chaddr[16]; /* client hardware address           */
  uint8_t sname[64];  /* server host name (unused)         */
  uint8_t file[128];  /* boot file (unused)                */
  uint32_t magic;     /* 0x63825363                        */
  uint8_t options[308];
} __attribute__((packed));

/* ---------------------------------------------------------------------------
 * Module-level state — one context per monitor process
 *
 * Droidspaces runs a separate monitor process per container, so a single
 * global context is sufficient and avoids any cross-container state sharing.
 * ---------------------------------------------------------------------------*/

typedef struct {
  int sock;
  char iface[IFNAMSIZ];
  uint32_t offer_ip_be; /* container IP, network byte order */
  uint32_t gw_ip_be;    /* DS_NAT_GW_IP, network byte order  */
  uint32_t netmask_be;  /* 255.255.0.0 for /16               */
  uint32_t dns1_be;
  uint32_t dns2_be;
  uint8_t peer_mac[6]; /* only respond to this client MAC */
  volatile sig_atomic_t stop;
  pthread_t tid;
} ds_dhcp_ctx_t;

static ds_dhcp_ctx_t g_dhcp;
static pthread_mutex_t g_dhcp_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------------------------------------------------------------------------
 * Option helpers
 * ---------------------------------------------------------------------------*/

/* Append a single DHCP option into buf[*pos].  Returns 0 on success. */
static int opt_put(uint8_t *buf, int *pos, int buflen, uint8_t code,
                   uint8_t len, const void *data) {
  if (*pos + 2 + (int)len > buflen)
    return -1;
  buf[(*pos)++] = code;
  buf[(*pos)++] = len;
  memcpy(buf + *pos, data, len);
  *pos += (int)len;
  return 0;
}

static int opt_put_u8(uint8_t *buf, int *pos, int buflen, uint8_t code,
                      uint8_t v) {
  return opt_put(buf, pos, buflen, code, 1, &v);
}

static int opt_put_u32(uint8_t *buf, int *pos, int buflen, uint8_t code,
                       uint32_t v_be) {
  return opt_put(buf, pos, buflen, code, 4, &v_be);
}

/* Find option `code` in the options blob.  Returns length found, or -1. */
static int opt_get(const uint8_t *opts, int opts_len, uint8_t code,
                   uint8_t *out, int max_len) {
  int i = 0;
  while (i < opts_len) {
    uint8_t c = opts[i++];
    if (c == OPT_END)
      break;
    if (c == OPT_PAD)
      continue;
    if (i >= opts_len)
      break;
    uint8_t l = opts[i++];
    if (i + (int)l > opts_len)
      break;
    if (c == code) {
      int copy = ((int)l < max_len) ? (int)l : max_len;
      memcpy(out, opts + i, (size_t)copy);
      return (int)l;
    }
    i += (int)l;
  }
  return -1;
}

/* ---------------------------------------------------------------------------
 * Reply builder
 *
 * Constructs a DHCPOFFER or DHCPACK into *reply.
 * Returns the total packet length to send.
 * ---------------------------------------------------------------------------*/

static int build_reply(struct dhcp_pkt *reply, const struct dhcp_pkt *req,
                       uint8_t msg_type, const ds_dhcp_ctx_t *ctx) {
  memset(reply, 0, sizeof(*reply));

  reply->op = BOOTP_REPLY;
  reply->htype = HTYPE_ETHERNET;
  reply->hlen = req->hlen;
  reply->hops = 0;
  reply->xid = req->xid;     /* echo client's transaction id */
  reply->flags = req->flags; /* preserve BROADCAST flag      */
  reply->ciaddr = 0;
  reply->yiaddr = ctx->offer_ip_be;
  reply->siaddr = ctx->gw_ip_be;
  reply->giaddr = 0;
  memcpy(reply->chaddr, req->chaddr, sizeof(reply->chaddr));
  reply->magic = htonl(DHCP_MAGIC);

  int pos = 0;
  int blen = (int)sizeof(reply->options);

  opt_put_u8(reply->options, &pos, blen, OPT_MSG_TYPE, msg_type);
  opt_put_u32(reply->options, &pos, blen, OPT_SERVER_ID, ctx->gw_ip_be);
  opt_put_u32(reply->options, &pos, blen, OPT_LEASE_TIME,
              htonl(DHCP_LEASE_SEC));
  opt_put_u32(reply->options, &pos, blen, OPT_RENEWAL_T1, htonl(DHCP_T1_SEC));
  opt_put_u32(reply->options, &pos, blen, OPT_REBIND_T2, htonl(DHCP_T2_SEC));
  opt_put_u32(reply->options, &pos, blen, OPT_SUBNET_MASK, ctx->netmask_be);
  opt_put_u32(reply->options, &pos, blen, OPT_ROUTER, ctx->gw_ip_be);

  /* DNS: up to two servers packed as a single option */
  if (ctx->dns1_be) {
    uint8_t dns_buf[8];
    int dns_len = 0;
    memcpy(dns_buf, &ctx->dns1_be, 4);
    dns_len += 4;
    if (ctx->dns2_be) {
      memcpy(dns_buf + 4, &ctx->dns2_be, 4);
      dns_len += 4;
    }
    opt_put(reply->options, &pos, blen, OPT_DNS, (uint8_t)dns_len, dns_buf);
  }

  reply->options[pos++] = OPT_END;

  return (int)offsetof(struct dhcp_pkt, options) + pos;
}

/* ---------------------------------------------------------------------------
 * Reply transmitter
 *
 * Always broadcasts to 255.255.255.255:68.  The veth pair is a private
 * point-to-point link so broadcast reaches only the container — no ARP
 * dependency during initial address acquisition.
 * ---------------------------------------------------------------------------*/

static int send_reply(int sock, const struct dhcp_pkt *pkt, int pkt_len) {
  struct sockaddr_in dst;
  memset(&dst, 0, sizeof(dst));
  dst.sin_family = AF_INET;
  dst.sin_port = htons(DHCP_CLIENT_PORT);
  dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);

  ssize_t sent = sendto(sock, pkt, (size_t)pkt_len, 0, (struct sockaddr *)&dst,
                        sizeof(dst));
  if (sent < 0) {
    ds_warn("[DHCP] sendto: %s", strerror(errno));
    return -1;
  }
  return 0;
}

/* ---------------------------------------------------------------------------
 * Core server loop (runs as joinable thread)
 * ---------------------------------------------------------------------------*/

static void *dhcp_server_loop(void *arg) {
  ds_dhcp_ctx_t *ctx = (ds_dhcp_ctx_t *)arg;

  char offer_str[INET_ADDRSTRLEN];
  struct in_addr tmp_addr;
  tmp_addr.s_addr = ctx->offer_ip_be;
  if (!inet_ntop(AF_INET, &tmp_addr, offer_str, sizeof(offer_str)))
    offer_str[0] = '\0';

  ds_log("DHCP Server started on %s  offer=%s", ctx->iface, offer_str);

  struct dhcp_pkt req;
  struct dhcp_pkt reply;

  while (!ctx->stop) {
    /* ── Receive ──────────────────────────────────────────────────────── */
    ssize_t len = recv(ctx->sock, &req, sizeof(req), 0);
    if (len < 0) {
      if (ctx->stop)
        break;
      if (errno == EINTR || errno == EAGAIN)
        continue;
      ds_warn("[DHCP] recv: %s", strerror(errno));
      break;
    }

    /* Minimum viable: fixed fields + magic */
    if (len < (ssize_t)offsetof(struct dhcp_pkt, options))
      continue;

    if (ntohl(req.magic) != DHCP_MAGIC)
      continue;

    if (req.op != BOOTP_REQUEST)
      continue;

    int opts_len = (int)(len - (ssize_t)offsetof(struct dhcp_pkt, options));

    /* MAC filter — drop packets from any client that isn't our container.
     * In bridge mode all monitors share ds-br0 and receive every container's
     * broadcasts; without this check each monitor races to answer the wrong
     * client, causing IP misassignment under multi-container setups. */
    if (memcmp(req.chaddr, ctx->peer_mac, 6) != 0)
      continue;

    uint8_t type_byte = 0;
    if (opt_get(req.options, opts_len, OPT_MSG_TYPE, &type_byte, 1) < 0)
      continue;

    /* ── Dispatch ─────────────────────────────────────────────────────── */
    switch (type_byte) {

    case DHCPDISCOVER:
      ds_log("[DHCP] DISCOVER  xid=%08x  chaddr=%02x:%02x:%02x:%02x:%02x:%02x",
             ntohl(req.xid), req.chaddr[0], req.chaddr[1], req.chaddr[2],
             req.chaddr[3], req.chaddr[4], req.chaddr[5]);
      {
        int plen = build_reply(&reply, &req, DHCPOFFER, ctx);
        if (send_reply(ctx->sock, &reply, plen) == 0)
          ds_log("[DHCP] OFFER    → %s  xid=%08x", offer_str, ntohl(req.xid));
      }
      break;

    case DHCPREQUEST: {
      /*
       * If the client included a server-identifier option, it must match
       * our gateway IP — otherwise the REQUEST is directed at another server.
       */
      uint8_t sid[4];
      if (opt_get(req.options, opts_len, OPT_SERVER_ID, sid, 4) == 4) {
        uint32_t sid_be;
        memcpy(&sid_be, sid, 4);
        if (sid_be != ctx->gw_ip_be) {
          ds_log("[DHCP] REQUEST  for other server — ignoring");
          break;
        }
      }

      ds_log("[DHCP] REQUEST   xid=%08x  chaddr=%02x:%02x:%02x:%02x:%02x:%02x",
             ntohl(req.xid), req.chaddr[0], req.chaddr[1], req.chaddr[2],
             req.chaddr[3], req.chaddr[4], req.chaddr[5]);

      int plen = build_reply(&reply, &req, DHCPACK, ctx);
      if (send_reply(ctx->sock, &reply, plen) == 0)
        ds_log("[DHCP] ACK      → %s  xid=%08x", offer_str, ntohl(req.xid));
      break;
    }

    default:
      /* DHCPRELEASE, DHCPINFORM, etc. — nothing to do */
      break;
    }
  }

  close(ctx->sock);
  ctx->sock = -1;
  ds_log("[DHCP] Server stopped on %s", ctx->iface);
  return NULL;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------*/

void ds_dhcp_server_start(struct ds_config *cfg, const char *veth_host,
                          uint32_t offer_ip_be, uint32_t gw_ip_be,
                          const uint8_t peer_mac[6]) {
  pthread_mutex_lock(&g_dhcp_lock);

  memset(&g_dhcp, 0, sizeof(g_dhcp));
  g_dhcp.sock = -1;
  g_dhcp.offer_ip_be = offer_ip_be;
  g_dhcp.gw_ip_be = gw_ip_be;
  g_dhcp.netmask_be = htonl(0xFFFF0000u); /* /16 */
  g_dhcp.stop = 0;
  strncpy(g_dhcp.iface, veth_host, IFNAMSIZ - 1);
  memcpy(g_dhcp.peer_mac, peer_mac, 6);

  /* Resolve DNS to advertise in the DHCP lease.
   *
   * NAT mode without --dns → the DNS proxy is running on 172.28.0.1:53.
   * Offer that as the sole nameserver so the container's DHCP client writes
   * "nameserver 172.28.0.1" into its resolv.conf automatically - consistent
   * with what fix_networking_rootfs() already wrote for static init systems.
   *
   * Any other case → use the explicit --dns servers or fall back to the
   * compiled-in defaults. */
  if (cfg && cfg->net_mode == DS_NET_NAT && !cfg->dns_servers[0]) {
    g_dhcp.dns1_be = inet_addr(DS_NAT_GW_IP);
    g_dhcp.dns2_be = 0; /* single server is enough - proxy does the rest */
  } else {
    g_dhcp.dns1_be = inet_addr(DS_DNS_DEFAULT_1);
    g_dhcp.dns2_be = inet_addr(DS_DNS_DEFAULT_2);
    if (cfg && cfg->dns_servers[0]) {
      char tmp[256];
      strncpy(tmp, cfg->dns_servers, sizeof(tmp) - 1);
      tmp[sizeof(tmp) - 1] = '\0';
      char *saveptr = NULL;
      char *tok = strtok_r(tmp, ", ", &saveptr);
      if (tok) {
        in_addr_t a = inet_addr(tok);
        if (a != (in_addr_t)(-1))
          g_dhcp.dns1_be = (uint32_t)a;
      }
      tok = strtok_r(NULL, ", ", &saveptr);
      if (tok) {
        in_addr_t a = inet_addr(tok);
        if (a != (in_addr_t)(-1))
          g_dhcp.dns2_be = (uint32_t)a;
      }
    }
  }

  /* ── Create UDP socket ─────────────────────────────────────────────── */
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ds_warn("[DHCP] socket: %s", strerror(errno));
    goto unlock;
  }

  int one = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

  if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) < 0) {
    ds_warn("[DHCP] SO_BROADCAST: %s", strerror(errno));
    close(sock);
    goto unlock;
  }

  /*
   * Bind to the specific veth_host interface only.
   * This prevents any collision with DHCP servers on other host interfaces.
   * Requires root (Droidspaces always runs as root).
   */
  if (setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, veth_host,
                 (socklen_t)(strlen(veth_host) + 1)) < 0) {
    ds_warn("[DHCP] SO_BINDTODEVICE(%s): %s", veth_host, strerror(errno));
    close(sock);
    goto unlock;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(DHCP_SERVER_PORT);
  addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ds_warn("[DHCP] bind(port 67): %s", strerror(errno));
    close(sock);
    goto unlock;
  }

  g_dhcp.sock = sock;

  /* ── Spawn joinable thread ─────────────────────────────────────────── */
  /* Joinable (default) so ds_dhcp_server_stop() can pthread_join() and
   * guarantee the thread has fully exited before the next start() call
   * calls memset(&g_dhcp, 0).  A detached thread could still be running
   * when memset fires, corrupting its own context mid-loop. */
  if (pthread_create(&g_dhcp.tid, NULL, dhcp_server_loop, &g_dhcp) != 0) {
    ds_warn("[DHCP] pthread_create: %s", strerror(errno));
    close(sock);
    g_dhcp.sock = -1;
  }

unlock:
  pthread_mutex_unlock(&g_dhcp_lock);
}

void ds_dhcp_server_stop(void) {
  pthread_mutex_lock(&g_dhcp_lock);
  g_dhcp.stop = 1;
  pthread_t tid = g_dhcp.tid;
  if (g_dhcp.sock >= 0) {
    /*
     * shutdown() unblocks the recv() call in dhcp_server_loop without
     * closing the fd — the thread closes it after the loop exits.
     * This mirrors the same pattern used by ds_net_stop_route_monitor().
     */
    shutdown(g_dhcp.sock, SHUT_RDWR);
  }
  pthread_mutex_unlock(&g_dhcp_lock);

  /* Join outside the lock — guarantees the thread has fully exited and
   * closed ctx->sock before ds_dhcp_server_start() calls memset(&g_dhcp).
   * Without this, a reboot cycle races: start() overwrites g_dhcp.stop=0
   * while the old thread is still spinning, causing two threads to serve
   * DHCP simultaneously with a corrupted shared context. */
  if (tid != 0)
    pthread_join(tid, NULL);
}
