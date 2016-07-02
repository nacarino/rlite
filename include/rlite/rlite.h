/*
 * librlite API for applications, core functionalities.
 *
 * Copyright (C) 2015-2016 Nextworks
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

#ifndef __RLITE_CTRL_H__
#define __RLITE_CTRL_H__

#include <stdlib.h>
#include <stdint.h>
#include "rlite/kernel-msg.h"
#include "rlite/uipcps-msg.h"
#include "rlite/utils.h"

#include "list.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Some words about thead-safety. The current implementation is **not**
 * thread-safe with respect to concurrent access to the same rlite_ctrl struct.
 * Turning it into a thread-safe one would be easy, but it would also
 * require a dependency on the pthread library. The current decision is
 * therefore to let the user care about concurrency, and use locks if
 * needed.
 */

struct rl_ctrl {
    /* File descriptor for the RLITE control device ("/dev/rlite") */
    int rfd;

    /* Private fields follow. Don't access them from outside the library. */

    /* A FIFO queue that stores expired events, that can be
     * returned when user calls rl_ctrl_wait() or rl_ctrl_wait_any(). */
    struct list_head pqueue;

    /* What event-id to use for the next request issued to the kernel. */
    uint32_t event_id_counter;

    /* Flags used in the ioctl(). */
    unsigned int flags;
};

uint32_t
rl_ctrl_get_id(struct rl_ctrl *ctrl);

void
rl_flow_spec_default(struct rl_flow_spec *spec);

void
rl_flow_cfg_default(struct rl_flow_config *cfg);

int
rl_open_appl_port(rl_port_t port_id);

int
rl_open_mgmt_port(rl_ipcp_id_t ipcp_id);

int
rl_write_msg(int rfd, struct rl_msg_base *msg);

int
rl_ctrl_init(struct rl_ctrl *ctrl, const char *dev,
             unsigned flags);

int
rl_ctrl_fini(struct rl_ctrl *ctrl);

/* Asynchronous API. */

uint32_t
rl_ctrl_fa_req(struct rl_ctrl *ctrl, const char *dif_name,
               const struct rina_name *local_appl,
               const struct rina_name *remote_appl,
               const struct rl_flow_spec *flowspec);

uint32_t
rl_ctrl_reg_req(struct rl_ctrl *ctrl, int reg,
                const char *dif_name,
                const struct rina_name *appl_name);

struct rl_msg_base *
rl_ctrl_wait(struct rl_ctrl *ctrl, uint32_t event_id, unsigned int wait_ms);

struct rl_msg_base *
rl_ctrl_wait_any(struct rl_ctrl *ctrl, unsigned int msg_type,
                 unsigned int wait_ms);

/* Synchronous API (higher level, implemented by means of the
 * asynchronous API. */
int
rl_ctrl_flow_alloc(struct rl_ctrl *ctrl, const char *dif_name,
                   const struct rina_name *local_appl,
                   const struct rina_name *remote_appl,
                   const struct rl_flow_spec *flowspec);

int
rl_ctrl_register(struct rl_ctrl *ctrl, const char *dif_name,
                 const struct rina_name *appl_name);

int
rl_ctrl_unregister(struct rl_ctrl *ctrl, const char *dif_name,
                   const struct rina_name *appl_name);

int
rl_ctrl_flow_accept(struct rl_ctrl *ctrl);

#ifdef __cplusplus
}
#endif

#endif  /* __RLITE_CTRL_H__ */
