/*
 * IPCP and flow management functionalities exported by the kernel.
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

#ifndef __RLITE_CONF_H__
#define __RLITE_CONF_H__

#include "rlite.h"
#include "list.h"


#ifdef __cplusplus
extern "C" {
#endif

struct rl_flow {
    /* Flow attributes. */
    rl_ipcp_id_t ipcp_id;
    rl_port_t local_port;
    rl_port_t remote_port;
    rl_addr_t local_addr;
    rl_addr_t remote_addr;

    struct list_head node;
};

long int
rl_conf_ipcp_create(struct rl_ctrl *ctrl,
                    const struct rina_name *name, const char *dif_type,
                    const char *dif_name);

int
rl_conf_ipcp_uipcp_wait(struct rl_ctrl *ctrl, rl_ipcp_id_t ipcp_id);

int
rl_conf_ipcp_destroy(struct rl_ctrl *ctrl, rl_ipcp_id_t ipcp_id);

int
rl_conf_ipcp_config(struct rl_ctrl *ctrl, rl_ipcp_id_t ipcp_id,
                    const char *param_name, const char *param_value);

/* Fetch information about all flows in the system. */
int
rl_conf_flows_print(struct rl_ctrl *ctrl, struct list_head *flows);

int
rl_conf_flows_fetch(struct rl_ctrl *ctrl, struct list_head *flows);

void
rl_conf_flows_purge(struct list_head *flows);

#ifdef __cplusplus
}
#endif

#endif  /* __RLITE_CONF_H__ */
