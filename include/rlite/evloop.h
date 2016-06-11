/*
 * Extensible event-loop over an rlite control device.
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

#ifndef __RLITE_EVLOOP_H__
#define __RLITE_EVLOOP_H__

#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include "rlite/kernel-msg.h"
#include "rlite/uipcps-msg.h"
#include "rlite/utils.h"

#include "list.h"
#include "rlite.h"


#ifdef __cplusplus
extern "C" {
#endif

struct rl_evloop;

/* The signature of a response handler. */
typedef int (*rl_resp_handler_t)(struct rl_evloop *loop,
                                    const struct rl_msg_base *b_resp,
                                    const struct rl_msg_base *b_req);

/* The signature of file descriptor callback. */
typedef void (*rl_evloop_fdcb_t)(struct rl_evloop *loop, int fd);

/* The signature of timer callback. */
typedef void (*rl_tmr_cb_t)(struct rl_evloop *loop, void *arg);

struct rl_evloop {
    struct rl_ctrl ctrl;

    /* Handler for the event loop thread. */
    pthread_t evloop_th;

    /* Is the evloop running already?. */
    int running;

    /* Table containing the kernel handlers. */
    rl_resp_handler_t handlers[RLITE_KER_MSG_MAX+1];

    /* Synchronization variables used to implement mutual exclusion between the
     * event-loop thread and the user thead. */
    pthread_mutex_t lock;

    /* Used to stop the event-loop. */
    int eventfd;

    /* Used to store the list of file descriptor callbacks registered within
     * the event-loop. */
    struct list_head fdcbs;

    struct list_head timer_events;
    pthread_mutex_t timer_lock;
    int timer_events_cnt;
    int timer_next_id;

    rl_resp_handler_t usr_ipcp_update;
};

/* Issue a request message to the kernel. Takes the ownership of
 * @msg. */
struct rl_msg_base *
rl_evloop_issue_request(struct rl_evloop *loop, struct rl_msg_base *msg,
                        size_t msg_len, int has_response,
                        unsigned int wait_for_completion, int *result);

int
rl_evloop_stop(struct rl_evloop *loop);

int
rl_evloop_join(struct rl_evloop *loop);

int
rl_evloop_init(struct rl_evloop *loop, const char *dev,
               rl_resp_handler_t *handlers,
               unsigned int flags);

int
rl_evloop_fini(struct rl_evloop *loop);

int
rl_evloop_set_handler(struct rl_evloop *loop, unsigned int index,
                         rl_resp_handler_t handler);

int
rl_evloop_fdcb_add(struct rl_evloop *loop, int fd,
                      rl_evloop_fdcb_t cb);

int
rl_evloop_fdcb_del(struct rl_evloop *loop, int fd);

int
rl_evloop_schedule(struct rl_evloop *loop, unsigned long delta_ms,
                      rl_tmr_cb_t cb, void *arg);

int
rl_evloop_schedule_canc(struct rl_evloop *loop, int id);

struct rl_kmsg_appl_register_resp *
rl_evloop_reg_req(struct rl_evloop *loop, uint32_t event_id,
                  unsigned int wait_ms,
                  int reg, const char *dif_name,
                  const struct rina_name *appl_name);

int rl_evloop_register(struct rl_evloop *loop,
                       int reg, const char *dif_name,
                       const struct rina_name *appl_name,
                       unsigned int wait_ms);

int rl_evloop_flow_alloc(struct rl_evloop *loop,
                        uint32_t event_id,
                        const char *dif_name,
                        const struct rina_name *local_appl,
                        const struct rina_name *remote_appl,
                        const struct rl_flow_spec *flowcfg,
                        rl_ipcp_id_t upper_ipcp_id,
                        rl_port_t *port_id, unsigned int wait_ms);

int rl_evloop_fa_resp(struct rl_evloop *loop,
                      uint32_t kevent_id, rl_ipcp_id_t ipcp_id,
                      rl_ipcp_id_t upper_ipcp_id, rl_port_t port_id,
                      uint8_t response);

int rl_evloop_ipcp_config(struct rl_evloop *loop, rl_ipcp_id_t ipcp_id,
                          const char *param_name, const char *param_value);

#ifdef __cplusplus
}
#endif

#endif  /* __RLITE_EVLOOP_H__ */
