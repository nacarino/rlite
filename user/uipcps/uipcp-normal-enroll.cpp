/*
 * Enrollment support for normal uipcps.
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

#include <unistd.h>
#include <cassert>
#include <pthread.h>

#include "uipcp-normal.hpp"

using namespace std;


/* Timeout intervals are expressed in milliseconds. */
#define NEIGH_KEEPALIVE_INTVAL      5000
#define NEIGH_KEEPALIVE_THRESH      3
#define NEIGH_ENROLL_TO             1500
#define NEIGH_ENROLL_MAX_ATTEMPTS   3

NeighFlow::NeighFlow(Neighbor *n, const string& supdif,
                     unsigned int pid, int ffd, unsigned int lid) :
                                  neigh(n), supp_dif(supdif),
                                  port_id(pid), flow_fd(ffd),
                                  lower_ipcp_id(lid), conn(NULL),
                                  enroll_tmrid(0),
                                  enrollment_state(NEIGH_NONE),
                                  keepalive_tmrid(0),
                                  pending_keepalive_reqs(0)
{
    pthread_cond_init(&enrollment_stopped, NULL);
    assert(neigh);
}

NeighFlow::~NeighFlow()
{
    int ret;

    if (!neigh) {
        /* This is an empty instance. */
        return;
    }

    enroll_tmr_stop();
    keepalive_tmr_stop();

    if (conn) {
        delete conn;
    }

    ret = close(flow_fd);

    if (ret) {
        UPE(neigh->rib->uipcp, "Error deallocating N-1 flow fd %d\n",
                flow_fd);
    } else {
        UPD(neigh->rib->uipcp, "N-1 flow deallocated [fd=%d]\n",
                flow_fd);
    }

    uipcps_lower_flow_removed(neigh->rib->uipcp->uipcps,
                              neigh->rib->uipcp->id,
                              lower_ipcp_id);

    pthread_cond_destroy(&enrollment_stopped);
}

int
NeighFlow::send_to_port_id(CDAPMessage *m, int invoke_id,
                          const UipcpObject *obj) const
{
    char objbuf[4096];
    int objlen;
    char *serbuf = NULL;
    size_t serlen = 0;
    int ret;

    if (obj) {
        objlen = obj->serialize(objbuf, sizeof(objbuf));
        if (objlen < 0) {
            UPE(neigh->rib->uipcp, "serialization failed\n");
            return objlen;
        }

        m->set_obj_value(objbuf, objlen);
    }

    try {
        ret = conn->msg_ser(m, invoke_id, &serbuf, &serlen);
    } catch (std::bad_alloc) {
        ret = -1;
    }

    if (ret) {
        UPE(neigh->rib->uipcp, "message serialization failed\n");
        if (serbuf) {
            delete [] serbuf;
        }
        return -1;
    }

    ret = mgmt_write_to_local_port(neigh->rib->uipcp, port_id,
                                   serbuf, serlen);

    if (serbuf) {
        delete [] serbuf;
    }

    return ret;
}

bool
NeighFlow::enrollment_starting(const CDAPMessage *rm) const
{
    return enrollment_state == NEIGH_S_WAIT_START &&
           rm->op_code == gpb::M_START &&
           rm->obj_name == obj_name::enrollment &&
           rm->obj_class == obj_class::enrollment;
}

void
NeighFlow::abort_enrollment()
{
    CDAPMessage m;
    int ret;

    UPE(neigh->rib->uipcp, "Aborting enrollment\n");

    if (enrollment_state == NEIGH_NONE) {
        return;
    }

    enrollment_state = NEIGH_NONE;

    m.m_release(gpb::F_NO_FLAGS);

    ret = send_to_port_id(&m, 0, NULL);
    if (ret) {
        UPE(neigh->rib->uipcp, "send_to_port_id() failed\n");
    }

    if (conn) {
        conn->reset();
    }

    if (neigh->initiator &&
                ++neigh->enroll_attempts < NEIGH_ENROLL_MAX_ATTEMPTS) {
        /* Retry the enrollment. */
        PI("Enrollment aborted, trying again [attempt #%d]\n",
           neigh->enroll_attempts + 1);
        neigh->enroll_fsm_run(this, NULL);

    } else {
        /* Give up. */
        neigh->enroll_attempts = 0;
        pthread_cond_signal(&enrollment_stopped);
    }
}

static void
keepalive_timeout_cb(struct rl_evloop *loop, void *arg)
{
    NeighFlow *nf = static_cast<NeighFlow *>(arg);
    uipcp_rib *rib = nf->neigh->rib;
    ScopeLock(rib->lock);
    CDAPMessage m;
    int ret;

    nf->keepalive_tmrid = 0;

    UPV(rib->uipcp, "Sending keepalive M_READ to neighbor '%s'\n",
        static_cast<string>(nf->neigh->ipcp_name).c_str());

    m.m_read(gpb::F_NO_FLAGS, obj_class::keepalive, obj_name::keepalive,
             0, 0, string());

    ret = nf->send_to_port_id(&m, 0, NULL);
    if (ret) {
        UPE(rib->uipcp, "send_to_port_id() failed\n");

    } else {
        nf->pending_keepalive_reqs++;
    }

    if (nf->pending_keepalive_reqs > NEIGH_KEEPALIVE_THRESH) {
        bool delete_neighbor;
        RinaName neigh_name = nf->neigh->ipcp_name;

        /* We assume the neighbor is not alive on this flow.
         * Remove the NeighFlow from the Neighbor and, if the
         * NeighFlow is the current mgmt flow, elect
         * another NeighFlow as mgmt flow, if possible. */
        UPI(rib->uipcp, "Neighbor %s is not alive on N-1 flow %u "
            "and therefore will be pruned\n",
            static_cast<string>(neigh_name).c_str(), nf->port_id);

        nf->neigh->flows.erase(nf->port_id);

        if (nf->port_id == nf->neigh->mgmt_port_id && nf->neigh->flows.size())
        {
            nf->neigh->mgmt_port_id = nf->neigh->flows.begin()->second->port_id;
            UPI(rib->uipcp, "Mgmt flow for neigh %s switches to port id %u\n",
                static_cast<string>(neigh_name).c_str(),
                nf->neigh->mgmt_port_id);
        }

        delete_neighbor = (nf->neigh->flows.size() == 0);

        /* First delete the N-1 flow. */
        delete nf;

        /* If this was the last N-1 flow, delete the neighbor. */
        if (delete_neighbor) {
            rib->del_neighbor(neigh_name);
        }

    } else {
        /* Schedule the next keepalive request. */
        nf->keepalive_tmr_start();
    }
}

static void
enroll_timeout_cb(struct rl_evloop *loop, void *arg)
{
    NeighFlow *nf = static_cast<NeighFlow *>(arg);
    ScopeLock(nf->neigh->rib->lock);

    UPI(nf->neigh->rib->uipcp, "Enrollment timeout with neighbor '%s'\n",
        static_cast<string>(nf->neigh->ipcp_name).c_str());

    nf->abort_enrollment();
}

void
NeighFlow::enroll_tmr_start()
{
    enroll_tmrid = rl_evloop_schedule(&neigh->rib->uipcp->loop,
                                      NEIGH_ENROLL_TO,
                                      enroll_timeout_cb, this);
}

void
NeighFlow::enroll_tmr_stop()
{
    if (enroll_tmrid > 0) {
        rl_evloop_schedule_canc(&neigh->rib->uipcp->loop, enroll_tmrid);
        enroll_tmrid = 0;
    }
}

void
NeighFlow::keepalive_tmr_start()
{
    keepalive_tmrid = rl_evloop_schedule(&neigh->rib->uipcp->loop,
                                         NEIGH_KEEPALIVE_INTVAL,
                                         keepalive_timeout_cb, this);
}

void
NeighFlow::keepalive_tmr_stop()
{
    if (keepalive_tmrid > 0) {
        rl_evloop_schedule_canc(&neigh->rib->uipcp->loop, keepalive_tmrid);
        keepalive_tmrid = 0;
    }
}

Neighbor::Neighbor(struct uipcp_rib *rib_, const struct rina_name *name,
                   bool initiator_)
{
    rib = rib_;
    initiator = initiator_;
    enroll_attempts = 0;
    ipcp_name = RinaName(name);
    memset(enroll_fsm_handlers, 0, sizeof(enroll_fsm_handlers));
    mgmt_port_id = -1;
    enroll_fsm_handlers[NEIGH_NONE] = &Neighbor::none;
    enroll_fsm_handlers[NEIGH_I_WAIT_CONNECT_R] = &Neighbor::i_wait_connect_r;
    enroll_fsm_handlers[NEIGH_S_WAIT_START] = &Neighbor::s_wait_start;
    enroll_fsm_handlers[NEIGH_I_WAIT_START_R] = &Neighbor::i_wait_start_r;
    enroll_fsm_handlers[NEIGH_S_WAIT_STOP_R] = &Neighbor::s_wait_stop_r;
    enroll_fsm_handlers[NEIGH_I_WAIT_STOP] = &Neighbor::i_wait_stop;
    enroll_fsm_handlers[NEIGH_I_WAIT_START] = &Neighbor::i_wait_start;
    enroll_fsm_handlers[NEIGH_ENROLLED] = &Neighbor::enrolled;
}

Neighbor::~Neighbor()
{
    for (map<rl_port_t, NeighFlow *>::iterator mit = flows.begin();
                                            mit != flows.end(); mit++) {
        delete mit->second;
    }
}

const char *
Neighbor::enrollment_state_repr(state_t s) const
{
    switch (s) {
        case NEIGH_NONE:
            return "NONE";

        case NEIGH_I_WAIT_CONNECT_R:
            return "I_WAIT_CONNECT_R";

        case NEIGH_S_WAIT_START:
            return "S_WAIT_START";

        case NEIGH_I_WAIT_START_R:
            return "I_WAIT_START_R";

        case NEIGH_S_WAIT_STOP_R:
            return "S_WAIT_STOP_R";

        case NEIGH_I_WAIT_STOP:
            return "I_WAIT_STOP";

        case NEIGH_I_WAIT_START:
            return "I_WAIT_START";

        case NEIGH_ENROLLED:
            return "ENROLLED";

        default:
            assert(0);
    }

    return NULL;
}

const NeighFlow *
Neighbor::_mgmt_conn() const
{
    map<rl_port_t, NeighFlow*>::const_iterator mit;

    mit = flows.find(mgmt_port_id);
    assert(mit != flows.end());

    return mit->second;
}

NeighFlow *
Neighbor::mgmt_conn()
{
    const NeighFlow *nf = _mgmt_conn();
    return const_cast<NeighFlow *>(nf);
}

int
Neighbor::none(NeighFlow *nf, const CDAPMessage *rm)
{
    CDAPMessage m;
    int ret;
    state_t next_state;
    int invoke_id = 0;

    if (rm == NULL) {
        /* (1) I --> S: M_CONNECT */

        CDAPAuthValue av;
        struct rina_name dst_name;

        rina_name_fill(&dst_name, ipcp_name.apn.c_str(),
                       ipcp_name.api.c_str(), ipcp_name.aen.c_str(),
                        ipcp_name.aei.c_str());

        /* We are the enrollment initiator, let's send an
         * M_CONNECT message. */
        nf->conn = new CDAPConn(nf->flow_fd, 1);

        ret = m.m_connect(gpb::AUTH_NONE, &av, &rib->uipcp->name,
                          &dst_name);
        rina_name_free(&dst_name);

        if (ret) {
            UPE(rib->uipcp, "M_CONNECT creation failed\n");
            nf->abort_enrollment();
            return -1;
        }

        next_state = NEIGH_I_WAIT_CONNECT_R;

    } else {
        /* (1) S <-- I: M_CONNECT
         * (2) S --> I: M_CONNECT_R */

        /* We are the enrollment slave, let's send an
         * M_CONNECT_R message. */
        assert(rm->op_code == gpb::M_CONNECT); /* Rely on CDAP fsm. */
        ret = m.m_connect_r(rm, 0, string());
        if (ret) {
            UPE(rib->uipcp, "M_CONNECT_R creation failed\n");
            nf->abort_enrollment();
            return -1;
        }

        invoke_id = rm->invoke_id;

        next_state = NEIGH_S_WAIT_START;
    }

    ret = nf->send_to_port_id(&m, invoke_id, NULL);
    if (ret) {
        UPE(rib->uipcp, "send_to_port_id() failed\n");
        nf->abort_enrollment();
        return 0;
    }

    nf->enroll_tmr_start();
    nf->enrollment_state = next_state;

    return 0;
}

int
Neighbor::i_wait_connect_r(NeighFlow *nf, const CDAPMessage *rm)
{
    /* (2) I <-- S: M_CONNECT_R
     * (3) I --> S: M_START */
    EnrollmentInfo enr_info;
    CDAPMessage m;
    int ret;

    assert(rm->op_code == gpb::M_CONNECT_R); /* Rely on CDAP fsm. */

    if (rm->result) {
        UPE(rib->uipcp, "Neighbor returned negative response [%d], '%s'\n",
           rm->result, rm->result_reason.c_str());
        nf->abort_enrollment();
        return 0;
    }

    m.m_start(gpb::F_NO_FLAGS, obj_class::enrollment, obj_name::enrollment,
              0, 0, string());

    enr_info.address = rib->uipcp->addr;
    enr_info.lower_difs = rib->lower_difs;

    ret = nf->send_to_port_id(&m, 0, &enr_info);
    if (ret) {
        UPE(rib->uipcp, "send_to_port_id() failed\n");
        nf->abort_enrollment();
        return 0;
    }

    nf->enroll_tmr_stop();
    nf->enroll_tmr_start();
    nf->enrollment_state = NEIGH_I_WAIT_START_R;

    return 0;
}

int
Neighbor::s_wait_start(NeighFlow *nf, const CDAPMessage *rm)
{
    /* (3) S <-- I: M_START
     * (4) S --> I: M_START_R
     * (5) S --> I: M_CREATE
     * (6) S --> I: M_STOP */
    const char *objbuf;
    size_t objlen;
    bool has_address;
    int ret;

    if (rm->op_code != gpb::M_START) {
        UPE(rib->uipcp, "M_START expected\n");
        nf->abort_enrollment();
        return 0;
    }

    if (rm->obj_class != obj_class::enrollment ||
            rm->obj_name != obj_name::enrollment) {
        UPE(rib->uipcp, "%s:%s object expected\n",
            obj_name::enrollment.c_str(), obj_class::enrollment.c_str());
        nf->abort_enrollment();
        return 0;
    }

    rm->get_obj_value(objbuf, objlen);
    if (!objbuf) {
        UPE(rib->uipcp, "M_START does not contain a nested message\n");
        nf->abort_enrollment();
        return 0;
    }

    EnrollmentInfo enr_info(objbuf, objlen);
    CDAPMessage m;

    has_address = (enr_info.address != 0);

    if (!has_address) {
        /* Assign an address to the initiator. */
        enr_info.address = rib->address_allocate();
    }

    NeighborCandidate cand;
    /* We have to add the initiator to the set of candidate neighbors here,
     * because this is needed for neighbor address look-up (necessary when
     * creating the lower-flow entry for the initiator). */
    cand.apn = ipcp_name.apn;
    cand.api = ipcp_name.api;
    cand.address = enr_info.address;
    cand.lower_difs = enr_info.lower_difs;
    rib->neighbors_seen[static_cast<string>(ipcp_name)] = cand;
    rib->neighbors_cand.insert(static_cast<string>(ipcp_name));

    NeighborCandidateList ncl;
     /* We also need to propagate the new NeighborCandidate object to the other
     * neighbors already enrolled. */
    ncl.candidates.push_back(cand);
    rib->remote_sync_obj_excluding(nf->neigh, true, obj_class::neighbors,
                                   obj_name::neighbors, &ncl);

    m.m_start_r(gpb::F_NO_FLAGS, 0, string());
    m.obj_class = obj_class::enrollment;
    m.obj_name = obj_name::enrollment;

    ret = nf->send_to_port_id(&m, rm->invoke_id, &enr_info);
    if (ret) {
        UPE(rib->uipcp, "send_to_port_id() failed\n");
        nf->abort_enrollment();
        return 0;
    }

    if (has_address) {
        /* Send DIF static information. */
    }

    /* Send only a neighbor representing myself, because it's
     * required by the initiator to add_lower_flow(). */
    RinaName cand_name;

    cand = NeighborCandidate();
    cand_name = RinaName(&rib->uipcp->name);
    cand.apn = cand_name.apn;
    cand.api = cand_name.api;
    cand.address = rib->uipcp->addr;
    cand.lower_difs = rib->lower_difs;
    ncl.candidates.clear();
    ncl.candidates.push_back(cand);

    remote_sync_obj(nf, true, obj_class::neighbors, obj_name::neighbors, &ncl);

    /* Stop the enrollment. */
    enr_info.start_early = true;

    m = CDAPMessage();
    m.m_stop(gpb::F_NO_FLAGS, obj_class::enrollment, obj_name::enrollment,
             0, 0, string());

    ret = nf->send_to_port_id(&m, 0, &enr_info);
    if (ret) {
        UPE(rib->uipcp, "send_to_port_id() failed\n");
        nf->abort_enrollment();
        return 0;
    }

    nf->enroll_tmr_stop();
    nf->enroll_tmr_start();
    nf->enrollment_state = NEIGH_S_WAIT_STOP_R;

    return 0;
}

int
Neighbor::i_wait_start_r(NeighFlow *nf, const CDAPMessage *rm)
{
    /* (4) I <-- S: M_START_R */
    const char *objbuf;
    size_t objlen;

    if (rm->op_code != gpb::M_START_R) {
        UPE(rib->uipcp, "M_START_R expected\n");
        nf->abort_enrollment();
        return 0;
    }

    if (rm->obj_class != obj_class::enrollment ||
            rm->obj_name != obj_name::enrollment) {
        UPE(rib->uipcp, "%s:%s object expected\n",
            obj_name::enrollment.c_str(), obj_class::enrollment.c_str());
        nf->abort_enrollment();
        return 0;
    }

    if (rm->result) {
        UPE(rib->uipcp, "Neighbor returned negative response [%d], '%s'\n",
           rm->result, rm->result_reason.c_str());
        nf->abort_enrollment();
        return 0;
    }

    rm->get_obj_value(objbuf, objlen);
    if (!objbuf) {
        UPE(rib->uipcp, "M_START_R does not contain a nested message\n");
        nf->abort_enrollment();
        return 0;
    }

    EnrollmentInfo enr_info(objbuf, objlen);

    /* The slave may have specified an address for us. */
    if (enr_info.address) {
        rib->set_address(enr_info.address);
    }

    nf->enroll_tmr_stop();
    nf->enroll_tmr_start();
    nf->enrollment_state = NEIGH_I_WAIT_STOP;

    return 0;
}

int
Neighbor::i_wait_stop(NeighFlow *nf, const CDAPMessage *rm)
{
    /* (6) I <-- S: M_STOP
     * (7) I --> S: M_STOP_R */
    const char *objbuf;
    size_t objlen;
    CDAPMessage m;
    int ret;

    /* Here M_CREATE messages from the slave are accepted and
     * dispatched to the rib. */
    if (rm->op_code == gpb::M_CREATE) {
        return rib->cdap_dispatch(rm, nf);
    }

    if (rm->op_code != gpb::M_STOP) {
        UPE(rib->uipcp, "M_STOP expected\n");
        nf->abort_enrollment();
        return 0;
    }

    if (rm->obj_class != obj_class::enrollment ||
            rm->obj_name != obj_name::enrollment) {
        UPE(rib->uipcp, "%s:%s object expected\n",
            obj_name::enrollment.c_str(), obj_class::enrollment.c_str());
        nf->abort_enrollment();
        return 0;
    }

    rm->get_obj_value(objbuf, objlen);
    if (!objbuf) {
        UPE(rib->uipcp, "M_STOP does not contain a nested message\n");
        nf->abort_enrollment();
        return 0;
    }

    EnrollmentInfo enr_info(objbuf, objlen);

    /* Update our address according to what received from the
     * neighbor. */
    if (enr_info.address) {
        rib->set_address(enr_info.address);
    }

    /* If operational state indicates that we (the initiator) are already
     * DIF member, we can send our dynamic information to the slave. */

    /* Here we may M_READ from the slave. */

    m.m_stop_r(gpb::F_NO_FLAGS, 0, string());
    m.obj_class = obj_class::enrollment;
    m.obj_name = obj_name::enrollment;

    ret = nf->send_to_port_id(&m, rm->invoke_id, NULL);
    if (ret) {
        UPE(rib->uipcp, "send_to_port_id() failed\n");
        nf->abort_enrollment();
        return 0;
    }

    if (enr_info.start_early) {
        UPI(rib->uipcp, "Initiator is allowed to start early\n");
        nf->enroll_tmr_stop();
        nf->keepalive_tmr_start();
        nf->enrollment_state = NEIGH_ENROLLED;

        /* Add a new LowerFlow entry to the RIB, corresponding to
         * the new neighbor. */
        rib->commit_lower_flow(enr_info.address, *this);

        remote_sync_rib(nf);

        pthread_cond_signal(&nf->enrollment_stopped);

    } else {
        UPI(rib->uipcp, "Initiator is not allowed to start early\n");
        nf->enroll_tmr_stop();
        nf->enroll_tmr_start();
        nf->enrollment_state = NEIGH_I_WAIT_START;
    }

    return 0;
}

int
Neighbor::s_wait_stop_r(NeighFlow *nf, const CDAPMessage *rm)
{
    /* (7) S <-- I: M_STOP_R */
    /* (8) S --> I: M_START(status) */
    CDAPMessage m;
    int ret;

    if (rm->op_code != gpb::M_STOP_R) {
        UPE(rib->uipcp, "M_START_R expected\n");
        nf->abort_enrollment();
        return 0;
    }

    if (rm->result) {
        UPE(rib->uipcp, "Neighbor returned negative response [%d], '%s'\n",
           rm->result, rm->result_reason.c_str());
        nf->abort_enrollment();
        return 0;
    }

    /* This is not required if the initiator is allowed to start
     * early. */
    m.m_start(gpb::F_NO_FLAGS, obj_class::status, obj_name::status,
              0, 0, string());

    ret = nf->send_to_port_id(&m, 0, NULL);
    if (ret) {
        UPE(rib->uipcp, "send_to_port_id failed\n");
        nf->abort_enrollment();
        return ret;
    }

    nf->enroll_tmr_stop();
    nf->keepalive_tmr_start();
    nf->enrollment_state = NEIGH_ENROLLED;

    /* Add a new LowerFlow entry to the RIB, corresponding to
     * the new neighbor. */
    rib->commit_lower_flow(rib->uipcp->addr, *this);

    remote_sync_rib(nf);

    pthread_cond_signal(&nf->enrollment_stopped);

    return 0;
}

int
Neighbor::i_wait_start(NeighFlow *nf, const CDAPMessage *rm)
{
    /* Not yet implemented. */
    assert(false);
    return 0;
}

int
Neighbor::enrolled(NeighFlow *nf, const CDAPMessage *rm)
{
    if (rm->op_code == gpb::M_START && rm->obj_class == obj_class::status
                && rm->obj_name == obj_name::status) {
        /* This is OK, but we didn't need it, as
         * we started early. */
        UPI(rib->uipcp, "Ignoring M_START(status)\n");
        return 0;
    }

    /* We are enrolled to this neighbor, so we can dispatch its
     * CDAP message to the RIB. */
    return rib->cdap_dispatch(rm, nf);
}

/* Did we complete the enrollment procedure with the neighbor? */
bool
Neighbor::is_enrolled()
{
    return has_mgmt_flow() && mgmt_conn()->enrollment_state == NEIGH_ENROLLED;
}

int
Neighbor::enroll_fsm_run(NeighFlow *nf, const CDAPMessage *rm)
{
    state_t old_state = nf->enrollment_state;
    int ret;

    if (is_enrolled() && nf != mgmt_conn() && nf->enrollment_starting(rm)) {
        /* We thought we were already enrolled with this neighbor, but
         * he is trying to start again the enrollment procedure on a
         * different flow. We therefore assume that the neighbor
         * crashed before we could detect it, and select the new flow
         * as the management one. */
        UPI(rib->uipcp, "Switch management flow, port-id %u --> port-id %u\n",
                mgmt_conn()->port_id,
                nf->port_id);
        mgmt_port_id = nf->port_id;
    }

    assert(nf->enrollment_state >= NEIGH_NONE &&
           nf->enrollment_state < NEIGH_STATE_LAST);
    assert(enroll_fsm_handlers[nf->enrollment_state]);

    ret = (this->*(enroll_fsm_handlers[nf->enrollment_state]))(nf, rm);

    if (old_state != nf->enrollment_state) {
        UPI(rib->uipcp, "switching state %s --> %s\n",
            enrollment_state_repr(old_state),
            enrollment_state_repr(nf->enrollment_state));
    }

    return ret;
}

int Neighbor::remote_sync_obj(const NeighFlow *nf, bool create,
                              const string& obj_class,
                              const string& obj_name,
                              const UipcpObject *obj_value) const
{
    CDAPMessage m;
    int ret;

    if (!nf) {
        assert(has_mgmt_flow());
        nf = mgmt_conn();
    }

    if (create) {
        m.m_create(gpb::F_NO_FLAGS, obj_class, obj_name,
                   0, 0, "");

    } else {
        m.m_delete(gpb::F_NO_FLAGS, obj_class, obj_name,
                   0, 0, "");
    }

    ret = nf->send_to_port_id(&m, 0, obj_value);
    if (ret) {
        UPE(rib->uipcp, "send_to_port_id() failed\n");
    }

    return ret;
}

int Neighbor::remote_sync_rib(NeighFlow *nf) const
{
    unsigned int limit = 10; /* Hardwired for now, but at least we limit. */
    int ret = 0;

    UPD(rib->uipcp, "Starting RIB sync with neighbor '%s'\n",
        static_cast<string>(ipcp_name).c_str());

    {
        /* Synchronize lower flow database. */
        map< rl_addr_t, map< rl_addr_t, LowerFlow > >::iterator it;
        map< rl_addr_t, LowerFlow >::iterator jt;
        LowerFlowList lfl;

        it = rib->lfdb.begin();
        if (it == rib->lfdb.end()) {
            return 0;
        }
        jt = it->second.begin();

        for (;;) {
                    lfl.flows.push_back(jt->second);
                    jt ++;
                    if (jt == it->second.end()) {
                        if (++it != rib->lfdb.end()) {
                            jt = it->second.begin();
                        }
                    }

                    if (lfl.flows.size() >= limit || it == rib->lfdb.end()) {
                        ret |= remote_sync_obj(nf, true, obj_class::lfdb,
                                               obj_name::lfdb, &lfl);
                        lfl.flows.clear();
                        if (it == rib->lfdb.end()) {
                            break;
                        }
                    }
        }
    }

    {
        /* Synchronize Directory Forwarding Table. */
        for (map< string, DFTEntry >::iterator e = rib->dft.begin();
                                               e != rib->dft.end();) {
            DFTSlice dft_slice;

            while (dft_slice.entries.size() < limit && e != rib->dft.end()) {
                dft_slice.entries.push_back(e->second);
                e ++;
            }

            ret |= remote_sync_obj(nf, true, obj_class::dft, obj_name::dft,
                                   &dft_slice);
        }
    }

    {
        bool sent_myself = false;

        /* Scan all the neighbors I know about. */
        for (map<string, NeighborCandidate>::iterator cit =
                rib->neighbors_seen.begin();
                cit != rib->neighbors_seen.end();) {
            NeighborCandidateList ncl;

            if (!sent_myself) {
                NeighborCandidate cand;
                RinaName cand_name;

                /* A neighbor representing myself. */
                cand_name = RinaName(&rib->uipcp->name);
                cand.apn = cand_name.apn;
                cand.api = cand_name.api;
                cand.address = rib->uipcp->addr;
                cand.lower_difs = rib->lower_difs;
                ncl.candidates.push_back(cand);

                sent_myself = true;
            }

            while (ncl.candidates.size() < limit &&
                            cit != rib->neighbors_seen.end()) {
                ncl.candidates.push_back(cit->second);
                cit ++;
            }

            ret |= remote_sync_obj(nf, true, obj_class::neighbors,
                                   obj_name::neighbors, &ncl);
        }
    }

    UPD(rib->uipcp, "Finished RIB sync with neighbor '%s'\n",
        static_cast<string>(ipcp_name).c_str());

    return ret;
}

void
sync_timeout_cb(struct rl_evloop *loop, void *arg)
{
    uipcp_rib *rib = static_cast<uipcp_rib *>(arg);
    ScopeLock(rib->lock);
    CDAPMessage m;

    UPV(rib->uipcp, "Syncing lower flows with neighbors\n");

    rib->remote_refresh_lower_flows();
    rib->sync_tmrid = rl_evloop_schedule(&rib->uipcp->loop,
					 RL_NEIGH_SYNC_INTVAL * 1000,
                                         sync_timeout_cb, rib);
}

int
uipcp_rib::remote_refresh_lower_flows()
{
    map< rl_addr_t, map< rl_addr_t, LowerFlow > >::iterator it;
    map< rl_addr_t, LowerFlow >::iterator jt;
    unsigned int limit = 10;
    int ret = 0;

    /* Fetch the map containing all the LFDB entries with the local
     * address corresponding to me. */
    it = lfdb.find(uipcp->addr);
    assert(it != lfdb.end());

    for (map< rl_addr_t, LowerFlow >::iterator jt = it->second.begin();
                                        jt != it->second.end();) {
        LowerFlowList lfl;

        while (lfl.flows.size() < limit && jt != it->second.end()) {
                jt->second.seqnum ++;
                lfl.flows.push_back(jt->second);
                jt ++;
        }
        ret |= remote_sync_obj_all(true, obj_class::lfdb,
				   obj_name::lfdb, &lfl);
    }

    return ret;
}

Neighbor *
uipcp_rib::get_neighbor(const struct rina_name *neigh_name, bool initiator)
{
    RinaName _neigh_name_(neigh_name);
    string neigh_name_s = static_cast<string>(_neigh_name_);

    if (!neighbors.count(neigh_name_s)) {
        neighbors[neigh_name_s] = new Neighbor(this, neigh_name, initiator);
    }

    return neighbors[neigh_name_s];
}

int
uipcp_rib::del_neighbor(const RinaName& neigh_name)
{
    map<string, Neighbor*>::iterator mit =
                    neighbors.find(static_cast<string>(neigh_name));

    assert(mit != neighbors.end());

    delete mit->second;
    neighbors.erase(mit);

    return 0;
}

rl_addr_t
uipcp_rib::lookup_neighbor_address(const RinaName& neigh_name) const
{
    map< string, NeighborCandidate >::const_iterator
            mit = neighbors_seen.find(static_cast<string>(neigh_name));

    if (mit != neighbors_seen.end()) {
        return mit->second.address;
    }

    return 0; /* Zero means no address was found. */
}

RinaName
uipcp_rib::lookup_neighbor_by_address(rl_addr_t address)
{
    map<string, NeighborCandidate>::iterator nit;

    for (nit = neighbors_seen.begin();
                        nit != neighbors_seen.end(); nit++) {
        if (nit->second.address == address) {
            return RinaName(nit->second.apn, nit->second.api,
                            string(), string());
        }
    }

    return RinaName();
}

static string
common_lower_dif(const list<string> l1, const list<string> l2)
{
    for (list<string>::const_iterator i = l1.begin(); i != l1.end(); i++) {
        for (list<string>::const_iterator j = l2.begin(); j != l2.end(); j++) {
            if (*i == *j) {
                return *i;
            }
        }
    }

    return string();
}

int
uipcp_rib::neighbors_handler(const CDAPMessage *rm, NeighFlow *nf)
{
    const char *objbuf;
    size_t objlen;
    bool propagate = false;
    bool add = true;

    if (rm->op_code != gpb::M_CREATE && rm->op_code != gpb::M_DELETE) {
        UPE(uipcp, "M_CREATE or M_DELETE expected\n");
        return 0;
    }

    if (rm->op_code == gpb::M_DELETE) {
        add = false;
    }

    rm->get_obj_value(objbuf, objlen);
    if (!objbuf) {
        UPE(uipcp, "M_START does not contain a nested message\n");
        nf->abort_enrollment();
        return 0;
    }

    NeighborCandidateList ncl(objbuf, objlen);
    NeighborCandidateList prop_ncl;
    RinaName my_name = RinaName(&uipcp->name);

    for (list<NeighborCandidate>::iterator neigh = ncl.candidates.begin();
                                neigh != ncl.candidates.end(); neigh++) {
        RinaName neigh_name = RinaName(neigh->apn, neigh->api, string(),
                                       string());
        string key = static_cast<string>(neigh_name);
        map< string, NeighborCandidate >::iterator
                                    mit = neighbors_seen.find(key);

        if (neigh_name == my_name) {
            /* Skip myself (as a neighbor of the slave). */
            continue;
        }

        if (add) {
            if (mit != neighbors_seen.end()) {
                /* We've already seen this one. */
                continue;
            }
            neighbors_seen[key] = *neigh;
            prop_ncl.candidates.push_back(*neigh);
            propagate = true;

            /* Check if it can be a candidate neighbor. */
            string common_dif = common_lower_dif(neigh->lower_difs,
                    lower_difs);
            if (common_dif == string()) {
                UPD(uipcp, "Neighbor %s discarded because there are no lower DIFs in "
                           "common with us\n", key.c_str());
            } else {
                neighbors_cand.insert(key);
                UPD(uipcp, "Candidate neighbor %s %s remotely\n", key.c_str(),
                        (mit != neighbors_seen.end() ? "updated" : "added"));
            }

        } else {
            if (mit == neighbors_seen.end()) {
                UPI(uipcp, "Candidate neighbor does not exist\n");
                continue;
            }

            /* Let's forget about this neighbor. */
            neighbors_seen.erase(mit);
            prop_ncl.candidates.push_back(*neigh);
            propagate = true;
            if (neighbors_cand.count(key)) {
                neighbors_cand.erase(key);
            }
            UPD(uipcp, "Candidate neighbor %s removed remotely\n",
                       key.c_str());
        }
    }

    if (propagate) {
        /* Propagate the updated information to the other neighbors,
         * so that they can update their Neighbor objects. */
        remote_sync_obj_excluding(nf->neigh, add, obj_class::neighbors,
                                  obj_name::neighbors, &prop_ncl);
    }

    return 0;
}

int
uipcp_rib::keepalive_handler(const CDAPMessage *rm, NeighFlow *nf)
{
    CDAPMessage m;
    int ret;

    if (rm->op_code != gpb::M_READ && rm->op_code != gpb::M_READ_R) {
        UPE(uipcp, "M_READ or M_READ_R expected\n");
        return 0;
    }

    if (rm->op_code == gpb::M_READ_R) {
        /* Reset the keepalive request counter, we know the neighbor
         * is alive on this flow. */
        nf->pending_keepalive_reqs = 0;

        UPV(uipcp, "M_READ_R(keepalive) received from neighbor %s\n",
            static_cast<string>(nf->neigh->ipcp_name).c_str());
        return 0;
    }

    /* Just reply back to tell the neighbor we are alive. */

    m.m_read_r(gpb::F_NO_FLAGS, obj_class::keepalive, obj_name::keepalive,
               0, 0, string());

    ret = nf->send_to_port_id(&m, rm->invoke_id, NULL);
    if (ret) {
        UPE(uipcp, "send_to_port_id() failed\n");
    }

    return 0;
}

int
uipcp_rib::lookup_neigh_flow_by_port_id(rl_port_t port_id,
                                        NeighFlow **nfp)
{
    *nfp = NULL;

    for (map<string, Neighbor*>::iterator nit = neighbors.begin();
                        nit != neighbors.end(); nit++) {
        Neighbor *neigh = nit->second;

        if (neigh->flows.count(port_id)) {
            *nfp = neigh->flows[port_id];
            assert((*nfp)->neigh);

            return 0;
        }
    }

    return -1;
}

int
Neighbor::alloc_flow(const char *supp_dif)
{
    struct rina_name neigh_name;
    rl_ipcp_id_t lower_ipcp_id_ = -1;
    rl_port_t port_id_;
    unsigned int event_id;
    int flow_fd_;
    int ret;

    if (has_mgmt_flow()) {
        UPI(rib->uipcp, "Trying to allocate additional N-1 flow\n");
    }

    ipcp_name.rina_name_fill(&neigh_name);

    {
        /* Lookup the id of the lower IPCP towards the neighbor. */
        struct uipcps *uipcps = rib->uipcp->uipcps;
        struct uipcp *cur;

        pthread_mutex_lock(&uipcps->lock);
        list_for_each_entry(cur, &uipcps->uipcps, node) {
            if (strcmp(cur->dif_name, supp_dif) == 0) {
                lower_ipcp_id_ = cur->id;
                break;
            }
        }
        pthread_mutex_unlock(&uipcps->lock);

        if (lower_ipcp_id_ == -1) {
            UPI(rib->uipcp, "Failed to get lower ipcp id in DIF %s\n",
			    supp_dif);
            return -1;
        }
    }

    event_id = rl_ctrl_get_id(&rib->uipcp->loop.ctrl);

    /* Allocate a flow for the enrollment. */
    ret = rl_evloop_flow_alloc(&rib->uipcp->loop, event_id, supp_dif,
                               &rib->uipcp->name, &neigh_name, NULL,
                               rib->uipcp->id, &port_id_, 2000);
    rina_name_free(&neigh_name);
    if (ret) {
        UPE(rib->uipcp, "Failed to allocate a flow towards neighbor\n");
        return -1;
    }

    flow_fd_ = rl_open_appl_port(port_id_);
    if (flow_fd_ < 0) {
        UPE(rib->uipcp, "Failed to access the flow towards the neighbor\n");
        return -1;
    }

    /* Set mgmt_port_id if required. */
    if (!has_mgmt_flow()) {
        mgmt_port_id = port_id_;
    }

    flows[port_id_] = new NeighFlow(this, string(supp_dif), port_id_, flow_fd_,
                                    lower_ipcp_id_);

    UPD(rib->uipcp, "N-1 flow allocated [fd=%d, port_id=%u]\n",
                    flows[port_id_]->flow_fd, flows[port_id_]->port_id);

    uipcps_lower_flow_added(rib->uipcp->uipcps, rib->uipcp->id,
                            lower_ipcp_id_);

    return 0;
}

int
normal_ipcp_enroll(struct uipcp *uipcp, const struct rl_cmsg_ipcp_enroll *req,
                   int wait_for_completion)
{
    uipcp_rib *rib = UIPCP_RIB(uipcp);
    Neighbor *neigh;
    NeighFlow *nf;
    int ret;

    pthread_mutex_lock(&rib->lock);

    neigh = rib->get_neighbor(&req->neigh_name, true);
    if (!neigh) {
        UPE(uipcp, "Failed to add neighbor\n");
        pthread_mutex_unlock(&rib->lock);
        return -1;
    }

    if (!neigh->has_mgmt_flow()) {
        ret = neigh->alloc_flow(req->supp_dif_name);
        if (ret) {
            pthread_mutex_unlock(&rib->lock);
            return ret;
        }
    }

    assert(neigh->has_mgmt_flow());

    nf = neigh->mgmt_conn();

    if (nf->enrollment_state != NEIGH_NONE) {
        UPI(rib->uipcp, "Enrollment already in progress, current state "
            "is %s\n", neigh->enrollment_state_repr(nf->enrollment_state));

    } else {
        /* Start the enrollment procedure as initiator. This will move
         * the internal state to  NEIGH_I_WAIT_CONNECT_R. */
        neigh->enroll_fsm_run(nf, NULL);
    }

    if (wait_for_completion) {
        /* Wait for the enrollment procedure to stop, either because of
         * successful completion (NEIGH_ENROLLED), or because of an abort
         * (NEIGH_NONE).
         */
        while (nf->enrollment_state != NEIGH_NONE &&
                nf->enrollment_state != NEIGH_ENROLLED) {
            pthread_cond_wait(&nf->enrollment_stopped, &rib->lock);
        }

        ret = nf->enrollment_state == NEIGH_ENROLLED ? 0 : -1;

    } else {
        ret = 0;
    }

    pthread_mutex_unlock(&rib->lock);

    return ret;
}

int
rib_neigh_set_port_id(struct uipcp_rib *rib,
                      const struct rina_name *neigh_name,
                      const char *supp_dif,
                      rl_port_t neigh_port_id,
                      rl_ipcp_id_t lower_ipcp_id)
{
    Neighbor *neigh = rib->get_neighbor(neigh_name, false);

    if (!neigh) {
        UPE(rib->uipcp, "Failed to get neighbor\n");
        return -1;
    }

    if (neigh->flows.count(neigh_port_id)) {
        UPE(rib->uipcp, "Port id '%u' already exists\n",
            neigh_port_id);
        return -1;
    }

    /* Set mgmt_port_id if required. */
    if (!neigh->has_mgmt_flow()) {
        neigh->mgmt_port_id = neigh_port_id;
    }

    neigh->flows[neigh_port_id] = new NeighFlow(neigh, string(supp_dif),
                                                neigh_port_id, 0,
                                                lower_ipcp_id);

    return 0;
}

int
rib_neigh_set_flow_fd(struct uipcp_rib *rib,
                      const struct rina_name *neigh_name,
                      rl_port_t neigh_port_id, int neigh_fd)
{
    Neighbor *neigh = rib->get_neighbor(neigh_name, false);

    if (!neigh) {
        UPE(rib->uipcp, "Failed to get neighbor\n");
    }

    if (!neigh->flows.count(neigh_port_id)) {
        UPE(rib->uipcp, "Port id '%u' does not exist\n",
            neigh_port_id);
        return -1;
    }

    neigh->flows[neigh_port_id]->flow_fd = neigh_fd;

    UPD(rib->uipcp, "N-1 flow allocated [fd=%d, port_id=%u]\n",
                    neigh->flows[neigh_port_id]->flow_fd,
                    neigh->flows[neigh_port_id]->port_id);

    return 0;
}

int
normal_get_enrollment_targets(struct uipcp *uipcp, struct list_head *neighs)
{
    uipcp_rib *rib = UIPCP_RIB(uipcp);
    struct enrolled_neigh *ni;
    ScopeLock(rib->lock);

    list_init(neighs);

    for (map<string, Neighbor*>::iterator nit = rib->neighbors.begin();
                        nit != rib->neighbors.end(); nit++) {
        Neighbor *neigh = nit->second;

        if (!neigh->initiator) {
            continue;
        }

        ni = static_cast<struct enrolled_neigh *>(malloc(sizeof(*ni)));
        if (!ni) {
            PE("Out of memory\n");
            return -1;
        }

        assert(uipcp->dif_name);
        assert(neigh->has_mgmt_flow());

        if (neigh->ipcp_name.rina_name_fill(&ni->neigh_name) ||
                rina_name_copy(&ni->ipcp_name, &uipcp->name) ||
                (ni->dif_name = strdup(uipcp->dif_name)) == NULL ||
                (ni->supp_dif = strdup(neigh->mgmt_conn()->
                                       supp_dif.c_str())) == NULL) {
            PE("Out of memory\n");
            free(ni);
            return -1;
        }
        list_add_tail(&ni->node, neighs);
    }

    return 0;
}
