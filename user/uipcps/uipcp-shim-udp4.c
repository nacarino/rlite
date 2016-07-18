/*
 * Management part of shim-udp4 IPCPs.
 *
 * Copyright (C) 2016 Nextworks
 * Author: Vincenzo Maffione <v.maffione@gmail.com>
 *
 * This file is part of rlite.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/udp.h>
#include <netinet/ip.h>

#include "rlite/list.h"
#include "uipcp-container.h"


#define RL_SHIM_UDP_PORT    0x0d1f

struct udp4_sdu {
    struct list_head    node;
    int                 len;
    uint8_t             buf[0];
};

/* Structure associated to a flow, contains information about the
 * remote UDP endpoint. */
struct udp4_endpoint {
    int                 fd;
    struct sockaddr_in  remote_addr;
    rl_port_t           port_id;
    uint32_t            kevent_id;
    int                 alloc_complete;
    struct list_head    sdus;

    struct list_head node;
};

/* Structure associated to a registered application or to a flow
 * requestor, contains an UDP socket bound to the IP address
 * corresponding to the application name. */
struct udp4_bindpoint {
    int fd;
    struct rina_name appl_name; /* Used to at unregister time. */
    rl_port_t port_id; /* Used at flow dealloc time. */
    struct rl_evloop *loop;
    struct list_head node;
};

struct shim_udp4 {
    struct uipcp        *uipcp;

    /* An UDP socket used to forward UDP packets to the receive queues
     * of endpoints. */
    int fwdfd;

    struct list_head    endpoints;
    struct list_head    bindpoints;
    uint32_t            kevent_id_cnt;
};

#define SHIM(_u)    ((struct shim_udp4 *)((_u)->priv))

static void
strrepchar(char *s, char old, char new)
{
    for (; *s != '\0'; s++) {
        if (*s == old) {
            *s = new;
        }
    }
}

/* Use socket API to translate a RINA name into an IP address. */
static int
rina_name_to_ipaddr(struct shim_udp4 *shim, const struct rina_name *name,
                    struct sockaddr_in *addr)
{
    struct addrinfo hints, *resaddrlist;
    char *name_s;
    int ret;

    name_s = rina_name_to_string(name);
    if (!name_s) {
        UPE(shim->uipcp, "Out of memory\n");
        return -1;
    }

    strrepchar(name_s, '/', '.');

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    ret = getaddrinfo(name_s, NULL, &hints, &resaddrlist);
    if (ret) {
        UPE(shim->uipcp, "getaddrinfo() failed: %s\n", gai_strerror(ret));
        goto err;
    }

    if (resaddrlist == NULL) {
        UPE(shim->uipcp, "Could not find IP address for %s\n", name_s);
        goto err;
    }

    /* Only consider the first element of the list. */
    memcpy(addr, resaddrlist->ai_addr, sizeof(*addr));
    freeaddrinfo(resaddrlist);
    {
        char strbuf[INET_ADDRSTRLEN];
        UPD(shim->uipcp, "'%s' --> '%s'\n", name_s,
            inet_ntop(AF_INET, &addr->sin_addr, strbuf, sizeof(strbuf)));
    }

    free(name_s);

    return 0;
err:
    free(name_s);
    return -1;
}

/* Use socket API to translate an IP address into a RINA name. */
static int
ipaddr_to_rina_name(struct shim_udp4 *shim, struct rina_name *name,
                    const struct sockaddr_in *addr)
{
    socklen_t hostlen = 256;
    char *host;
    int ret;

    host = malloc(hostlen);
    if (!host) {
        UPE(shim->uipcp, "Out of memory\n");
        return -1;
    }

    ret = getnameinfo((const struct sockaddr *)addr, sizeof(*addr),
                      host, hostlen, NULL, 0, 0);
    if (ret) {
        free(host);
        UPE(shim->uipcp, "getnameinfo() failed [%s]\n", gai_strerror(ret));
        return -1;
    }

    {
        char strbuf[INET_ADDRSTRLEN];
        UPD(shim->uipcp, "'%s' --> '%s'\n", inet_ntop(AF_INET, &addr->sin_addr,
            strbuf, sizeof(strbuf)), host);
    }

    strrepchar(host, '.', '/');

    ret = rina_name_from_string(host, name);
    free(host);
    if (ret) {
        UPE(shim->uipcp, "rina_name_from_string() failed\n");
    }

    return ret;
}

static void
udp4_flow_config_fill(struct udp4_endpoint *ep, struct rl_flow_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->fd = ep->fd;
    cfg->inet_ip = ep->remote_addr.sin_addr.s_addr;
    cfg->inet_port = ep->remote_addr.sin_port;
}

/* Lookup the specified remote IP address among the existing socket
 * endpoints. */
static struct udp4_endpoint *
udp4_endpoint_lookup(struct shim_udp4 *shim,
                     const struct sockaddr_in *remote_addr)
{
    struct udp4_endpoint *ep;

    list_for_each_entry(ep, &shim->endpoints, node) {
        if (remote_addr->sin_addr.s_addr == ep->remote_addr.sin_addr.s_addr) {
            return ep;
        }
    }

    return NULL;
}

/* Open an UDP socket and add it to the list of endpoints. */
static struct udp4_endpoint *
udp4_endpoint_open(struct shim_udp4 *shim)
{
    struct udp4_endpoint *ep = malloc(sizeof(*ep));
    struct sockaddr_in addr;

    if (!ep) {
        UPE(shim->uipcp, "Out of memory\n");
        return NULL;
    }
    memset(ep, 0, sizeof(*ep));

    list_init(&ep->sdus);

    ep->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ep->fd < 0) {
        UPE(shim->uipcp, "socket() failed [%d]\n", errno);
        free(ep);
        return NULL;
    }

    /* Ask the kernel to allocate an ephemeral UDP port for us. */
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(ep->fd, (const struct sockaddr *)&addr, sizeof(addr))) {
        UPE(shim->uipcp, "bind() failed [%d]\n", errno);
        close(ep->fd);
        free(ep);
        return NULL;
    }

    list_add_tail(&ep->node, &shim->endpoints);

    return ep;
}

static void
udp4_endpoint_close(struct udp4_endpoint *ep)
{
    struct udp4_sdu *sdu, *tmp;

    list_for_each_entry_safe(sdu, tmp, &ep->sdus, node) {
        list_del(&sdu->node);
        free(sdu);
    }

    close(ep->fd);
    list_del(&ep->node);
    free(ep);
}

static int
udp4_fwd_sdu(struct shim_udp4 *shim, struct udp4_endpoint *ep,
             const uint8_t *buf, int len)
{
    struct sockaddr_in dstaddr;
    socklen_t addrlen = sizeof(dstaddr);

    /* Forward the packet to the receive queue associated to ep->fd. */
    if (getsockname(ep->fd, (struct sockaddr *)&dstaddr, &addrlen)) {
        UPE(shim->uipcp, "getsockname() failed [%d]\n", errno);
        return -1;
    }

    dstaddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    {
        char strbuf[INET_ADDRSTRLEN];
        UPD(shim->uipcp, "Forwarding %d bytes to to %s:%u\n", len,
            inet_ntop(AF_INET, &dstaddr.sin_addr, strbuf, sizeof(strbuf)),
            ntohs(dstaddr.sin_port));
    }

    if (sendto(shim->fwdfd, buf, len, 0, (const struct sockaddr *)&dstaddr,
               sizeof(dstaddr)) < 0) {
        UPE(shim->uipcp, "sendto() failed [%d]\n", errno);
        return -1;
    }

    return 0;
}

static void
udp4_recv_dgram(struct rl_evloop *loop, int bfd)
{
    struct uipcp *uipcp = container_of(loop, struct uipcp, loop);
    struct shim_udp4 *shim = SHIM(uipcp);
    struct sockaddr_in remote_addr;
    socklen_t addrlen = sizeof(remote_addr);
    uint8_t payload[65536];
    struct udp4_endpoint *ep;
    int payload_len;

    /* Read the packet from the bound UDP socket. */
    payload_len = recvfrom(bfd, payload, sizeof(payload), 0,
                           (struct sockaddr *)&remote_addr, &addrlen);
    if (payload_len < 0) {
        UPE(uipcp, "recvfrom() failed [%d]\n", errno);
        return;
    }

    ep = udp4_endpoint_lookup(shim, &remote_addr);
    if (!ep) {
        struct rina_name remote_appl, local_appl;
        struct sockaddr_in bpaddr;
        struct rl_flow_config cfg;
        int ret;

        addrlen = sizeof(bpaddr);
        if (getsockname(bfd, (struct sockaddr *)&bpaddr, &addrlen)) {
            UPE(uipcp, "getsockname() failed [%d]\n", errno);
            return;
        }

        memset(&local_appl, 0, sizeof(local_appl));
        memset(&remote_appl, 0, sizeof(remote_appl));
        /* Lookup the local application from the packet destination IP
         * address. */
        if (ipaddr_to_rina_name(shim, &local_appl, &bpaddr)) {
            goto skip;
        }

        /* Lookup the remote application from the packet source IP address. */
        if (ipaddr_to_rina_name(shim, &remote_appl, &remote_addr)) {
            goto skip;
        }

        /* Open an UDP socket. */
        ep = udp4_endpoint_open(shim);
        if (!ep) {
            goto skip;
        }

        ep->kevent_id = shim->kevent_id_cnt++;
        memcpy(&ep->remote_addr, &remote_addr, sizeof(remote_addr));

        /* Push the file descriptor and source address down to kernelspace. */
        udp4_flow_config_fill(ep, &cfg);
        ret = uipcp_issue_fa_req_arrived(uipcp, ep->kevent_id, 0, 0, 0,
                                         &local_appl, &remote_appl, &cfg);
skip:
        rina_name_free(&local_appl);
        rina_name_free(&remote_appl);
        if (ret) {
            UPE(uipcp, "uipcp_fa_req_arrived() failed\n");
            return;
        }

        if (!ep) {
            UPE(uipcp, "Failed to create endpoint\n");
            return;
        }
    }

    if (!ep->alloc_complete) {
        /* Put the SDU in a temporary queue. */
        struct udp4_sdu *sdu;

        sdu = malloc(sizeof(*sdu) + payload_len);
        if (!sdu) {
            UPE(uipcp, "Out of memory\n");
            return;
        }

        sdu->len = payload_len;
        memcpy(sdu->buf, payload, payload_len);
        list_add_tail(&sdu->node, &ep->sdus);

        UPD(uipcp, "Queuing %d bytes\n", sdu->len);
    } else {
        if (ep->remote_addr.sin_port == htons(RL_SHIM_UDP_PORT)) {
            struct rl_flow_config cfg;

            /* We need to update the flow configuration in kernel-space. */
            ep->remote_addr.sin_port = remote_addr.sin_port;
            udp4_flow_config_fill(ep, &cfg);
            if (uipcp_issue_flow_cfg_update(shim->uipcp, ep->port_id, &cfg)) {
                UPE(shim->uipcp, "flow_cfg_update() failed\n");
                return;
            }
        }

        udp4_fwd_sdu(shim, ep, payload, payload_len);
    }
}

static struct udp4_bindpoint *
udp4_bindpoint_open(struct shim_udp4 *shim, struct rina_name *local_name)
{
    struct uipcp *uipcp = shim->uipcp;
    struct sockaddr_in bpaddr;
    struct udp4_bindpoint *bp;

    /* TODO We should update the DDNS here. For now we rely on
     *      static /etc/hosts configuration. */

    /* Look-up the IP address corresponding to the application name. */
    if (rina_name_to_ipaddr(shim, local_name, &bpaddr)) {
        return NULL;
    }

    bp = malloc(sizeof(*bp));
    if (!bp) {
        UPE(uipcp, "Out of memory\n");
        return NULL;
    }
    memset(bp, 0, sizeof(*bp));

    bp->loop = &uipcp->loop;

    /* Init the bound UDP socket, where implicit flow allocation
     * requests will be received for the req->appl_name. */
    bp->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (bp->fd < 0) {
        UPE(uipcp, "socket() failed [%d]\n", errno);
        goto err;
    }

    if (rina_name_copy(&bp->appl_name, local_name)) {
        goto err;
    }

    /* Bind to the UDP port reserved for incoming implicit flow allocations. */
    bpaddr.sin_port = htons(RL_SHIM_UDP_PORT);

    if (bind(bp->fd, (struct sockaddr *)&bpaddr, sizeof(bpaddr))) {
        UPE(uipcp, "bind() failed [%d]\n", errno);
        goto err;
    }

    /* The udp4_recv_dgram() callback will be invoked to receive UDP packets
     * for port 0x0D1F. */
    if (rl_evloop_fdcb_add(&uipcp->loop, bp->fd, udp4_recv_dgram)) {
        UPE(uipcp, "evloop_fdcb_add() failed\n");
        goto err;
    }

    list_add_tail(&bp->node, &shim->bindpoints);

    return bp;

err:
    rina_name_free(&bp->appl_name);
    if (bp->fd >= 0) close(bp->fd);
    free(bp);
    return NULL;
}

static void
udp4_bindpoint_close(struct udp4_bindpoint *bp)
{
    rl_evloop_fdcb_del(bp->loop, bp->fd);
    close(bp->fd);
    list_del(&bp->node);
    rina_name_free(&bp->appl_name);
    free(bp);
}

static struct udp4_endpoint *
get_endpoint_by_kevent_id(struct shim_udp4 *shim, uint32_t kevent_id)
{
    struct udp4_endpoint *ep;

    list_for_each_entry(ep, &shim->endpoints, node) {
        if (kevent_id == ep->kevent_id) {
            return ep;
        }
    }

    return NULL;
}

static int
shim_udp4_appl_register(struct rl_evloop *loop,
                        const struct rl_msg_base *b_resp,
                        const struct rl_msg_base *b_req)
{
    struct uipcp *uipcp = container_of(loop, struct uipcp, loop);
    struct rl_kmsg_appl_register *req =
                (struct rl_kmsg_appl_register *)b_resp;
    struct shim_udp4 *shim = SHIM(uipcp);
    struct udp4_bindpoint *bp;

    if (req->reg) {
        bp = udp4_bindpoint_open(shim, &req->appl_name);
        return uipcp_appl_register_resp(uipcp, uipcp->id,
                        bp ? RLITE_SUCC : RLITE_ERR, req);
    }

    list_for_each_entry(bp, &shim->bindpoints, node) {
        if (rina_name_cmp(&bp->appl_name, &req->appl_name) == 0) {
            udp4_bindpoint_close(bp);
            return 0;
        }
    }

    return -1;
}

static int
shim_udp4_fa_req(struct rl_evloop *loop,
                 const struct rl_msg_base *b_resp,
                 const struct rl_msg_base *b_req)
{
    struct uipcp *uipcp = container_of(loop, struct uipcp, loop);
    struct rl_kmsg_fa_req *req = (struct rl_kmsg_fa_req *)b_resp;
    struct shim_udp4 *shim = SHIM(uipcp);
    struct rl_flow_config cfg;
    struct udp4_bindpoint *bp;
    struct udp4_endpoint *ep;

    assert(b_req == NULL);

    UPD(uipcp, "[uipcp %u] Got reflected message\n", uipcp->id);

    /* Create the bindpoint to be able to complete the flow allocation. */
    bp = udp4_bindpoint_open(shim, &req->local_appl);
    if (!bp) {
        return -1;
    }

    /* Open an UDP socket. */
    ep = udp4_endpoint_open(shim);
    if (!ep) {
        udp4_bindpoint_close(bp);
        return -1;
    }

    ep->port_id = bp->port_id = req->local_port;

    /* Resolve the destination name into an IP address. */
    if (rina_name_to_ipaddr(shim, &req->remote_appl, &ep->remote_addr)) {
        udp4_bindpoint_close(bp);
        udp4_endpoint_close(ep);
        return -1;
    }

    /* The port will be the bindpoint one until we receive an UDP packet
     * from the other side. */
    ep->remote_addr.sin_port = htons(RL_SHIM_UDP_PORT);

    /* Issue a positive flow allocation response, pushing to the kernel
     * the socket file descriptor and the remote address. */
    udp4_flow_config_fill(ep, &cfg);
    uipcp_issue_fa_resp_arrived(uipcp, ep->port_id, 0, 0, 0, 0, &cfg);

    ep->alloc_complete = 1;

    return 0;
}

static int
shim_udp4_fa_resp(struct rl_evloop *loop,
                   const struct rl_msg_base *b_resp,
                   const struct rl_msg_base *b_req)
{
    struct uipcp *uipcp = container_of(loop, struct uipcp, loop);
    struct shim_udp4 *shim = SHIM(uipcp);
    struct rl_kmsg_fa_resp *resp = (struct rl_kmsg_fa_resp *)b_resp;
    struct udp4_sdu *sdu, *tmp;
    struct udp4_endpoint *ep;

    UPD(uipcp, "[uipcp %u] Got reflected message\n", uipcp->id);

    assert(b_req == NULL);

    ep = get_endpoint_by_kevent_id(shim, resp->kevent_id);
    if (!ep) {
        UPE(uipcp, "Cannot find endpoint corresponding to kevent-id '%d'\n",
                   resp->kevent_id);
        return 0;
    }

    ep->port_id = resp->port_id;


    if (resp->response) {
        /* Negative response, we have to close the endpoint. */
        UPD(uipcp, "Removing endpoint [port=%u,kevent_id=%u,sfd=%d]\n",
                ep->port_id, ep->kevent_id, ep->fd);
        udp4_endpoint_close(ep);

        return 0;
    }

    /* Response is positive, allocation is now complete. */
    ep->alloc_complete = 1;

    /* Foward any pending SDUs. */
    list_for_each_entry_safe(sdu, tmp, &ep->sdus, node) {
        udp4_fwd_sdu(shim, ep, sdu->buf, sdu->len);
        list_del(&sdu->node);
        free(sdu);
    }

    return 0;
}

static int
shim_udp4_flow_deallocated(struct rl_evloop *loop,
                       const struct rl_msg_base *b_resp,
                       const struct rl_msg_base *b_req)
{
    struct uipcp *uipcp = container_of(loop, struct uipcp, loop);
    struct rl_kmsg_flow_deallocated *req =
                (struct rl_kmsg_flow_deallocated *)b_resp;
    struct shim_udp4 *shim = SHIM(uipcp);
    struct udp4_bindpoint *bp;
    struct udp4_endpoint *ep;

    /* Close UDP endpoint and bindpoint associated to this flow. */

    list_for_each_entry(bp, &shim->bindpoints, node) {
        if (req->local_port_id == bp->port_id) {
            UPD(uipcp, "Removing bindpoint [port=%u,bfd=%d]\n",
                bp->port_id, bp->fd);
            udp4_bindpoint_close(bp);
            break;
        }
    }

    list_for_each_entry(ep, &shim->endpoints, node) {
        if (req->local_port_id == ep->port_id) {
            UPD(uipcp, "Removing endpoint [port=%u,kevent_id=%u,"
                "sfd=%d]\n", ep->port_id, ep->kevent_id, ep->fd);
            udp4_endpoint_close(ep);
            return 0;
        }
    }

    UPE(uipcp, "Cannot find endpoint corresponding to port '%d'\n",
               req->local_port_id);
    return -1;
}

static int
shim_udp4_init(struct uipcp *uipcp)
{
    struct shim_udp4 *shim;

    shim = malloc(sizeof(*shim));
    if (!shim) {
        UPE(uipcp, "Out of memory\n");
        return -1;
    }

    uipcp->priv = shim;
    shim->uipcp = uipcp;
    list_init(&shim->endpoints);
    list_init(&shim->bindpoints);
    shim->kevent_id_cnt = 1;

    shim->fwdfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (shim->fwdfd < 0) {
        UPE(shim->uipcp, "socket(SOCK_RAW failed [%d]\n)", errno);
        goto err;
    }

    return 0;
err:
    close(shim->fwdfd);
    return -1;
}

static int
shim_udp4_fini(struct uipcp *uipcp)
{
    struct shim_udp4 *shim = SHIM(uipcp);

    close(shim->fwdfd);

    {
        struct udp4_endpoint *ep, *tmp;

        list_for_each_entry_safe(ep, tmp, &shim->endpoints, node) {
            udp4_endpoint_close(ep);
        }
    }

    {
        struct udp4_bindpoint *bp, *tmp;

        list_for_each_entry_safe(bp, tmp, &shim->bindpoints, node) {
            udp4_bindpoint_close(bp);
        }
    }

    free(shim);

    return 0;
}

struct uipcp_ops shim_udp4_ops = {
    .init = shim_udp4_init,
    .fini = shim_udp4_fini,
    .appl_register = shim_udp4_appl_register,
    .fa_req = shim_udp4_fa_req,
    .fa_resp = shim_udp4_fa_resp,
    .flow_deallocated = shim_udp4_flow_deallocated,
};

