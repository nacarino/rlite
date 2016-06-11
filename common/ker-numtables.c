/*
 * Serialization tables for kernel control messages.
 *
 * Copyright (C) 2016 Vincenzo Maffione <v.maffione@gmail.com>
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

#include "rlite/utils.h"
#include "rlite/kernel-msg.h"


struct rl_msg_layout rl_ker_numtables[] = {
    [RLITE_KER_IPCP_CREATE] = {
        .copylen = sizeof(struct rl_kmsg_ipcp_create) -
                   sizeof(struct rina_name) - 2 * sizeof(char *),
        .names = 1,
        .strings = 2,
    },
    [RLITE_KER_IPCP_CREATE_RESP] = {
        .copylen = sizeof(struct rl_kmsg_ipcp_create_resp),
    },
    [RLITE_KER_IPCP_DESTROY] = {
        .copylen = sizeof(struct rl_kmsg_ipcp_destroy),
    },
    [RLITE_KER_FLOW_FETCH] = {
        .copylen = sizeof(struct rl_msg_base),
    },
    [RLITE_KER_FLOW_FETCH_RESP] = {
        .copylen = sizeof(struct rl_kmsg_flow_fetch_resp),
    },
    [RLITE_KER_IPCP_UPDATE] = {
        .copylen = sizeof(struct rl_kmsg_ipcp_update) -
                    1 * sizeof(struct rina_name) - 2 * sizeof(char *),
        .names = 1,
        .strings = 2,
    },
    [RLITE_KER_APPL_REGISTER] = {
        .copylen = sizeof(struct rl_kmsg_appl_register) -
                    1 * sizeof(struct rina_name) -
                    1 * sizeof(char *),
        .names = 1,
        .strings = 1,
    },
    [RLITE_KER_APPL_REGISTER_RESP] = {
        .copylen = sizeof(struct rl_kmsg_appl_register_resp) -
                    1 * sizeof(struct rina_name),
        .names = 1,
    },
    [RLITE_KER_FA_REQ] = {
        .copylen = sizeof(struct rl_kmsg_fa_req) -
                    2 * sizeof(struct rina_name) -
                    1 * sizeof(char *),
        .names = 2,
        .strings = 1,
    },
    [RLITE_KER_FA_RESP_ARRIVED] = {
        .copylen = sizeof(struct rl_kmsg_fa_resp_arrived),
    },
    [RLITE_KER_FA_RESP] = {
        .copylen = sizeof(struct rl_kmsg_fa_resp),
    },
    [RLITE_KER_FA_REQ_ARRIVED] = {
        .copylen = sizeof(struct rl_kmsg_fa_req_arrived) -
                    2 * sizeof(struct rina_name) - sizeof(char *),
        .names = 2,
        .strings = 1,
    },
    [RLITE_KER_IPCP_CONFIG] = {
        .copylen = sizeof(struct rl_kmsg_ipcp_config) -
                    2 * sizeof(char *),
        .strings = 2,
    },
    [RLITE_KER_IPCP_PDUFT_SET] = {
        .copylen = sizeof(struct rl_kmsg_ipcp_pduft_set),
    },
    [RLITE_KER_IPCP_PDUFT_FLUSH] = {
        .copylen = sizeof(struct rl_kmsg_ipcp_pduft_flush),
    },
    [RLITE_KER_IPCP_UIPCP_SET] = {
        .copylen = sizeof(struct rl_kmsg_ipcp_uipcp_set),
    },
    [RLITE_KER_UIPCP_FA_REQ_ARRIVED] = {
        .copylen = sizeof(struct rl_kmsg_uipcp_fa_req_arrived) -
                    2 * sizeof(struct rina_name),
        .names = 2,
    },
    [RLITE_KER_UIPCP_FA_RESP_ARRIVED] = {
        .copylen = sizeof(struct rl_kmsg_uipcp_fa_resp_arrived),
    },
    [RLITE_KER_FLOW_DEALLOCATED] = {
        .copylen = sizeof(struct rl_kmsg_flow_deallocated),
    },
    [RLITE_KER_FLOW_DEALLOC] = {
        .copylen = sizeof(struct rl_kmsg_flow_dealloc),
    },
    [RLITE_KER_IPCP_UIPCP_WAIT] = {
        .copylen = sizeof(struct rl_kmsg_ipcp_uipcp_wait),
    },
    [RLITE_KER_FLOW_STATS_REQ] = {
        .copylen = sizeof(struct rl_kmsg_flow_stats_req),
    },
    [RLITE_KER_FLOW_STATS_RESP] = {
        .copylen = sizeof(struct rl_kmsg_flow_stats_resp),
    },
    [RLITE_KER_MSG_MAX] = {
        .copylen = 0,
        .names = 0,
    },
};
