/*
 * Core implementation of normal uipcps.
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

#ifndef __UIPCP_RIB_H__
#define __UIPCP_RIB_H__

#include <string>
#include <map>
#include <list>
#include <pthread.h>

#include "rlite/common.h"
#include "rlite/utils.h"
#include "rlite/uipcps-msg.h"
#include "rlite/evloop.h"
#include "rlite/cdap.hpp"

#include "uipcp-normal-codecs.hpp"
#include "uipcp-container.h"

namespace obj_class {
    extern std::string adata;
    extern std::string dft;
    extern std::string neighbors;
    extern std::string enrollment;
    extern std::string status;
    extern std::string address;
    extern std::string lfdb; /* Lower Flow DB */
    extern std::string flows; /* Supported flows */
    extern std::string flow;
    extern std::string keepalive;
};

namespace obj_name {
    extern std::string adata;
    extern std::string dft;
    extern std::string neighbors;
    extern std::string enrollment;
    extern std::string status;
    extern std::string address;
    extern std::string lfdb;
    extern std::string whatevercast;
    extern std::string flows;
    extern std::string keepalive;
};

/* Time interval (in seconds) between two consecutive increments
 * of the age of LFDB entries. */

#define RL_AGE_INCR_INTERVAL    2

enum state_t {
    NEIGH_NONE = 0,
    NEIGH_I_WAIT_CONNECT_R,
    NEIGH_S_WAIT_START,
    NEIGH_I_WAIT_START_R,
    NEIGH_S_WAIT_STOP_R,
    NEIGH_I_WAIT_STOP,
    NEIGH_I_WAIT_START,
    NEIGH_ENROLLED,
    NEIGH_STATE_LAST,
};

struct Neighbor;

/* Holds the information about an N-1 flow towards a neighbor IPCP. */
struct NeighFlow {
    Neighbor *neigh;
    std::string supp_dif;
    rl_port_t port_id;
    int flow_fd;
    rl_ipcp_id_t lower_ipcp_id;
    CDAPConn *conn;

    int enroll_tmrid;
    int keepalive_tmrid;
    int pending_keepalive_reqs;
    pthread_cond_t enrollment_stopped;
    enum state_t enrollment_state;

    NeighFlow(Neighbor *n, const std::string& supp_dif, unsigned int pid,
              int ffd, unsigned int lid);
    ~NeighFlow();

    bool enrollment_starting(const CDAPMessage *m) const;
    void abort_enrollment();
    void enroll_tmr_start();
    void enroll_tmr_stop();
    void keepalive_tmr_start();
    void keepalive_tmr_stop();

    int send_to_port_id(CDAPMessage *m, int invoke_id,
                        const UipcpObject *obj) const;
};

/* Holds the information about a neighbor IPCP. */
struct Neighbor {
    /* Backpointer to the RIB. */
    struct uipcp_rib *rib;

    /* Name of the neighbor. */
    RinaName ipcp_name;

    /* Did we initiate the enrollment procedure towards this neighbor
     * or were we the target? */
    bool initiator;

    int enroll_attempts;

    std::map<rl_port_t, NeighFlow *> flows;
    rl_port_t mgmt_port_id;

    typedef int (Neighbor::*enroll_fsm_handler_t)(NeighFlow *nf,
                                                  const CDAPMessage *rm);
    enroll_fsm_handler_t enroll_fsm_handlers[NEIGH_STATE_LAST];

    Neighbor(struct uipcp_rib *rib, const struct rina_name *name,
             bool initiator);
    bool operator==(const Neighbor& other) const
        { return ipcp_name == other.ipcp_name; }
    bool operator!=(const Neighbor& other) const
        { return !(*this == other); }
    ~Neighbor();

    const char *enrollment_state_repr(state_t s) const;

    NeighFlow *mgmt_conn();
    bool has_mgmt_flow() const { return flows.size() > 0; }
    bool is_enrolled();
    int enroll_fsm_run(NeighFlow *nf, const CDAPMessage *rm);
    int alloc_flow(const char *supp_dif_name);

    /* Enrollment state machine handlers. */
    int none(NeighFlow *nf, const CDAPMessage *rm);
    int i_wait_connect_r(NeighFlow *nf, const CDAPMessage *rm);
    int s_wait_start(NeighFlow *nf, const CDAPMessage *rm);
    int i_wait_start_r(NeighFlow *nf, const CDAPMessage *rm);
    int i_wait_stop(NeighFlow *nf, const CDAPMessage *rm);
    int s_wait_stop_r(NeighFlow *nf, const CDAPMessage *rm);
    int i_wait_start(NeighFlow *nf, const CDAPMessage *rm);
    int enrolled(NeighFlow *nf, const CDAPMessage *rm);

    int remote_sync_obj(NeighFlow *nf, bool create,
                        const std::string& obj_class,
                        const std::string& obj_name,
                        const UipcpObject *obj_value) const;

    int remote_sync_rib(NeighFlow *nf) const;
};

/* Shortest Path algorithm. */
class SPEngine {
public:
    SPEngine() {};
    int run(rl_addr_t, const std::map<std::string, LowerFlow >& db);

    /* The routing table computed by run(). */
    std::map<rl_addr_t, rl_addr_t> next_hops;

private:
    struct Edge {
        rl_addr_t to;
        unsigned int cost;

        Edge(rl_addr_t to_, unsigned int cost_) :
                            to(to_), cost(cost_) { }
    };

    struct Info {
        unsigned int dist;
        bool visited;
    };

    std::map<rl_addr_t, std::list<Edge> > graph;
    std::map<rl_addr_t, Info> info;
};

class ScopeLock {
public:
    ScopeLock(pthread_mutex_t& m) : mutex(m) {
        pthread_mutex_lock(&mutex);
    }
    ~ScopeLock() {
        pthread_mutex_unlock(&mutex);
    }

private:
    pthread_mutex_t& mutex;
};

struct uipcp_rib {
    /* Backpointer to parent data structure. */
    struct uipcp *uipcp;

    int mgmtfd;

    /* RIB lock. */
    pthread_mutex_t lock;

    typedef int (uipcp_rib::*rib_handler_t)(const CDAPMessage *rm,
                                            NeighFlow *nf);
    std::map< std::string, rib_handler_t > handlers;

    /* Lower DIFs. */
    std::list< std::string > lower_difs;

    /* Neighbors. We keep track of all the NeighborCandidate objects seen,
     * even for candidates that have no lower DIF in common with us. This
     * is used to implement propagation of the CandidateNeighbors information,
     * so that all the IPCPs in the DIF know their potential candidate
     * neighbors.*/
    std::map< std::string, Neighbor* > neighbors;
    std::map< std::string, NeighborCandidate > neighbors_seen;
    std::set< std::string > neighbors_cand;

    /* Directory Forwarding Table. */
    std::map< std::string, DFTEntry > dft;

    /* Lower Flow Database. */
    std::map< std::string, LowerFlow > lfdb;

    SPEngine spe;

    /* For A-DATA messages. */
    InvokeIdMgr invoke_id_mgr;

    /* Supported flows. */
    std::map< std::string, FlowRequest > flow_reqs;
    std::map< unsigned int, FlowRequest > flow_reqs_tmp;

#ifdef RL_USE_QOS_CUBES
    /* Available QoS cubes. */
    std::map< std::string, struct rl_flow_config > qos_cubes;
#endif /* RL_USE_QOS_CUBES */

    /* Timer ID for age increment of LFDB entries. */
    int age_incr_tmrid;

    uipcp_rib(struct uipcp *_u);
    ~uipcp_rib();

    char *dump() const;

    int set_address(rl_addr_t address);
    Neighbor *get_neighbor(const struct rina_name *neigh_name, bool initiator);
    int del_neighbor(const RinaName& neigh_name);
    int dft_lookup(const RinaName& appl_name, rl_addr_t& dstaddr) const;
    int dft_set(const RinaName& appl_name, rl_addr_t remote_addr);
    int register_to_lower(int reg, std::string lower_dif);
    int appl_register(const struct rl_kmsg_appl_register *req);
    int flow_deallocated(struct rl_kmsg_flow_deallocated *req);
    rl_addr_t lookup_neighbor_address(const RinaName& neigh_name) const;
    RinaName lookup_neighbor_by_address(rl_addr_t address);
    int lookup_neigh_flow_by_port_id(rl_port_t port_id,
                                     NeighFlow **nfp);
    int commit_lower_flow(rl_addr_t local_addr, const Neighbor& neigh);
    int fa_req(struct rl_kmsg_fa_req *req);
    int fa_resp(struct rl_kmsg_fa_resp *resp);
    int pduft_sync();
    rl_addr_t address_allocate() const;

    int send_to_dst_addr(CDAPMessage *m, rl_addr_t dst_addr,
                         const UipcpObject *obj);

    /* Synchronize neighbors. */
    int remote_sync_obj_excluding(const Neighbor *exclude, bool create,
                              const std::string& obj_class,
                              const std::string& obj_name,
                              const UipcpObject *obj_value) const;
    int remote_sync_obj_all(bool create, const std::string& obj_class,
                        const std::string& obj_name,
                        const UipcpObject *obj_value) const;

    /* Receive info from neighbors. */
    int cdap_dispatch(const CDAPMessage *rm, NeighFlow *nf);

    /* RIB handlers for received CDAP messages. */
    int dft_handler(const CDAPMessage *rm, NeighFlow *nf);
    int neighbors_handler(const CDAPMessage *rm, NeighFlow *nf);
    int lfdb_handler(const CDAPMessage *rm, NeighFlow *nf);
    int flows_handler(const CDAPMessage *rm, NeighFlow *nf);
    int keepalive_handler(const CDAPMessage *rm, NeighFlow *nf);

    int flows_handler_create(const CDAPMessage *rm);
    int flows_handler_create_r(const CDAPMessage *rm);
    int flows_handler_delete(const CDAPMessage *rm);

private:
#ifdef RL_USE_QOS_CUBES
    int load_qos_cubes(const char *);
#endif /* RL_USE_QOS_CUBES */

    /* Id to be used with incoming flow allocation request. */
    uint32_t kevent_id_cnt;
};

int normal_ipcp_enroll(struct uipcp *uipcp,
                       const struct rl_cmsg_ipcp_enroll *req,
                       int wait_for_completion);

int normal_get_enrollment_targets(struct uipcp *uipcp,
                                  struct list_head *neighs);

int mgmt_write_to_local_port(struct uipcp *uipcp, rl_port_t local_port,
                             void *buf, size_t buflen);

int rib_neigh_set_port_id(struct uipcp_rib *rib,
                          const struct rina_name *neigh_name,
                          const char *supp_dif,
                          rl_port_t neigh_port_id,
                          rl_ipcp_id_t lower_ipcp_id);

int rib_neigh_set_flow_fd(struct uipcp_rib *rib,
                          const struct rina_name *neigh_name,
                          rl_port_t neigh_port_id, int neigh_fd);

void age_incr_cb(struct rl_evloop *loop, void *arg);

#define UIPCP_RIB(_u) ((uipcp_rib *)((_u)->priv))

#endif  /* __UIPCP_RIB_H__ */
