/*
 * RLITE normal IPC process
 *
 *    Vincenzo Maffione <v.maffione@gmail.it>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/types.h>
#include "rlite/utils.h"
#include "rlite-kernel.h"

#include <linux/module.h>
#include <linux/aio.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <linux/hashtable.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/delay.h>


#define PDUFT_HASHTABLE_BITS    3

struct rl_normal {
    struct ipcp_entry *ipcp;

    /* Implementation of the PDU Forwarding Table (PDUFT). */
    DECLARE_HASHTABLE(pdu_ft, PDUFT_HASHTABLE_BITS);

    rwlock_t pduft_lock;
};

static void *
rl_normal_create(struct ipcp_entry *ipcp)
{
    struct rl_normal *priv;

    priv = kzalloc(sizeof(*priv), GFP_KERNEL);
    if (!priv) {
        return NULL;
    }

    priv->ipcp = ipcp;
    hash_init(priv->pdu_ft);
    rwlock_init(&priv->pduft_lock);

    PD("New IPC created [%p]\n", priv);

    return priv;
}

static void
rl_normal_destroy(struct ipcp_entry *ipcp)
{
    struct rl_normal *priv = ipcp->priv;

    kfree(priv);

    PD("IPC [%p] destroyed\n", priv);
}

/* To be called under DTP lock */
static void
dtp_snd_reset(struct flow_entry *flow)
{
    struct fc_config *fc = &flow->cfg.dtcp.fc;
    struct dtp *dtp = &flow->dtp;

    dtp->flags |= DTP_F_DRF_SET;
    /* InitialSeqNumPolicy */
    dtp->next_seq_num_to_send = 0;
    dtp->snd_lwe = dtp->snd_rwe = dtp->next_seq_num_to_send;
    dtp->last_seq_num_sent = -1;
    dtp->next_snd_ctl_seq = 0;
    if (fc->fc_type == RLITE_FC_T_WIN) {
        dtp->snd_rwe += fc->cfg.w.initial_credit;
    }
}

/* To be called under DTP lock */
static void
dtp_rcv_reset(struct flow_entry *flow)
{
    struct fc_config *fc = &flow->cfg.dtcp.fc;
    struct dtp *dtp = &flow->dtp;

    dtp->flags |= DTP_F_DRF_EXPECTED;
    dtp->rcv_lwe = dtp->rcv_lwe_priv = dtp->rcv_rwe = 0;
    dtp->max_seq_num_rcvd = -1;
    dtp->last_snd_data_ack = 0;
    dtp->last_ctrl_seq_num_rcvd = 0;
    if (fc->fc_type == RLITE_FC_T_WIN) {
        dtp->rcv_rwe += fc->cfg.w.initial_credit;
    }
}

static void
snd_inact_tmr_cb(long unsigned arg)
{
    struct flow_entry *flow = (struct flow_entry *)arg;
    struct dtp *dtp = &flow->dtp;
    struct rl_buf *rb, *tmp;

    spin_lock_bh(&dtp->lock);

    dtp_dump(dtp);

    /* Re-initialize send-side state variables. */
    dtp_snd_reset(flow);

    /* Flush the retransmission queue. */
    PD("%s: dropping %u PDUs from rtxq\n", __func__, dtp->rtxq_len);
    list_for_each_entry_safe(rb, tmp, &dtp->rtxq, node) {
        list_del(&rb->node);
        rl_buf_free(rb);
        dtp->rtxq_len--;
    }

    /* Flush the closed window queue */
    PD("%s: dropping %u PDUs from cwq\n", __func__, dtp->cwq_len);
    list_for_each_entry_safe(rb, tmp, &dtp->cwq, node) {
        list_del(&rb->node);
        rl_buf_free(rb);
        dtp->cwq_len--;
    }

    /* Send control ack PDU */

    /* Send transfer PDU with zero length. */

    /* Notify user flow that there has been no activity for a while */

    spin_unlock_bh(&dtp->lock);

    /* Wake up processes sleeping on write(), since cwq and rtxq have been
     * emptied. */
    rl_write_restart_flow(flow);
}

static void
rcv_inact_tmr_cb(long unsigned arg)
{
    struct flow_entry *flow = (struct flow_entry *)arg;
    struct dtp *dtp = &flow->dtp;
    struct rl_buf *rb, *tmp;

    spin_lock_bh(&dtp->lock);

    /* Re-initialize receive-side state variables. */
    dtp_rcv_reset(flow);

    /* Flush sequencing queue. */
    PD("%s: dropping %u PDUs from seqq\n", __func__, dtp->seqq_len);
    list_for_each_entry_safe(rb, tmp, &dtp->seqq, node) {
        list_del(&rb->node);
        rl_buf_free(rb);
        dtp->seqq_len--;
    }

    spin_unlock_bh(&dtp->lock);
}

static int rmt_tx(struct ipcp_entry *ipcp, rl_addr_t remote_addr,
                  struct rl_buf *rb, bool maysleep);

void
rtx_tmr_cb(long unsigned arg)
{
    struct flow_entry *flow = (struct flow_entry *)arg;
    struct dtp *dtp = &flow->dtp;
    struct rl_buf *rb, *crb, *tmp;
    struct list_head rrbq;
    struct list_head *cur;

    PD("\n");

    INIT_LIST_HEAD(&rrbq);

    spin_lock_bh(&dtp->lock);

    if (timer_pending(&dtp->snd_inact_tmr)) {
        del_timer(&dtp->snd_inact_tmr);
    }

    if (likely(dtp->rtx_tmr_next)) {
        /* I couldn't figure out how to implement this with the macros in
         * list.h, so I went for a custom loop.
         * Here we scan the circular list starting from dtp->rtx_tmr_next,
         * rather than starting from the list head, so that we process
         * the PDU in ascending expiration time order. */
        for (cur = &dtp->rtx_tmr_next->node; 1; cur = cur->next) {
            if (cur == &dtp->rtxq) {
                /* This is the head, it's not contained in an rl_buf: let's
                 * skip it. */
                continue;
            }

            rb = list_entry(cur, struct rl_buf, node);

            if (jiffies >= rb->rtx_jiffies) {
                /* This rb should be retransmitted. */
                rb->rtx_jiffies += dtp->rtx_tmr_int;

                crb = rl_buf_clone(rb, GFP_ATOMIC);
                if (unlikely(!crb)) {
                    PE("Out of memory\n");
                } else {
                    list_add_tail(&crb->node, &rrbq);
                }

            } else {
                if (rb != dtp->rtx_tmr_next) {
                    NPD("Forward rtx timer by %u\n",
                            jiffies_to_msecs(rb->rtx_jiffies - jiffies));
                    dtp->rtx_tmr_next = rb;
                    mod_timer(&dtp->rtx_tmr, rb->rtx_jiffies);
                }
                break;
            }

            if (unlikely(cur->next == &dtp->rtx_tmr_next->node)) {
                /* This wrap around condition should never happen (it should be
                 * prevented by the else branch above), but it is technically
                 * possible, so it's better to check and exit the loop. */
                break;
            }
        }
    }

    spin_unlock_bh(&dtp->lock);

    /* Send PDUs popped out from RTX queue. Note that the rrbq list
     * is not emptied and must not be used after the scan. */
    list_for_each_entry_safe(crb, tmp, &rrbq, node) {
        struct rina_pci *pci = RLITE_BUF_PCI(crb);

        PD("sending [%lu] from rtxq\n",
                (long unsigned)pci->seqnum);
        rmt_tx(flow->txrx.ipcp, pci->dst_addr, crb, false);
    }

    spin_lock_bh(&dtp->lock);
    mod_timer(&dtp->snd_inact_tmr, jiffies + 3 * dtp->mpl_r_a);
    spin_unlock_bh(&dtp->lock);
}

static int rl_normal_sdu_rx_consumed(struct flow_entry *flow,
                                       struct rl_buf *rb);

#define RTX_MSECS_DEFAULT       1000
#define DATA_RXMS_MAX_DEFAULT   10
#define TKBK_INTVAL_MSEC        2

static int
rl_normal_flow_init(struct ipcp_entry *ipcp, struct flow_entry *flow)
{
    struct dtp *dtp = &flow->dtp;
    struct fc_config *fc = &flow->cfg.dtcp.fc;
    unsigned long mpl = 0;
    unsigned long r;

    dtp_snd_reset(flow);
    dtp_rcv_reset(flow);

    if (ipcp->dif) {
        mpl = msecs_to_jiffies(ipcp->dif->max_pdu_life);
    }

    if (!mpl) {
        PI("fixing MPL to %u ms\n", MPL_MSECS_DEFAULT);
        mpl = msecs_to_jiffies(MPL_MSECS_DEFAULT);
    }

    if (flow->cfg.dtcp.rtx_control && ! flow->cfg.dtcp.rtx.initial_tr) {
        PI("fixing initial_tr parameter to %u ms\n",
                RTX_MSECS_DEFAULT);
        flow->cfg.dtcp.rtx.initial_tr = RTX_MSECS_DEFAULT;
    }

    if (flow->cfg.dtcp.rtx_control && ! flow->cfg.dtcp.rtx.data_rxms_max) {
        PI("fixing data_rxms_max parameter to %u\n",
                DATA_RXMS_MAX_DEFAULT);
        flow->cfg.dtcp.rtx.data_rxms_max = DATA_RXMS_MAX_DEFAULT;
    }

    r = msecs_to_jiffies(flow->cfg.dtcp.rtx.initial_tr) *
                                    flow->cfg.dtcp.rtx.data_rxms_max;

    /* MPL + R + A */
    dtp->mpl_r_a = mpl + r + msecs_to_jiffies(flow->cfg.dtcp.initial_a);

    dtp->snd_inact_tmr.function = snd_inact_tmr_cb;
    dtp->snd_inact_tmr.data = (unsigned long)flow;

    dtp->rcv_inact_tmr.function = rcv_inact_tmr_cb;
    dtp->rcv_inact_tmr.data = (unsigned long)flow;

    dtp->rtx_tmr.function = rtx_tmr_cb;
    dtp->rtx_tmr.data = (unsigned long)flow;
    dtp->rtx_tmr_next = NULL;
    dtp->rtx_tmr_int = msecs_to_jiffies(flow->cfg.dtcp.rtx.initial_tr);

    if (fc->fc_type == RLITE_FC_T_WIN) {
        dtp->max_cwq_len = fc->cfg.w.max_cwq_len;
    }

    if (flow->cfg.dtcp.rtx_control) {
        dtp->max_rtxq_len = 64;  /* For now it's static. */
    }

    if (flow->cfg.dtcp.rtx_control || flow->cfg.dtcp.flow_control) {
        flow->sdu_rx_consumed = rl_normal_sdu_rx_consumed;
        NPD("flow->sdu_rx_consumed set\n");
    }

    if (flow->cfg.dtcp.bandwidth) {
        /* With the following definitions:
         *      R := requested bandwidth in bps
         *      M := token bucket timer period in milliseconds
         *      B := refill amount in bytes for each timer period
         * the following holds
         *      B = R * M / 8000
         * If the bandwidth is large enough, we can choose M = TKBK_INTVAL_MSEC
         * and will be B >= 250. If B is not large enough, we choose a larger
         * M, in such a way that B is still 250.
         */
        if (flow->cfg.dtcp.bandwidth < 4000) {
            /* We don't accept to provide less than 4 Kbps, so that intval_ms
             * can be always <= 500 milliseconds. */
            flow->cfg.dtcp.bandwidth = 4000;
        }
        if (flow->cfg.dtcp.bandwidth < (2000000 / TKBK_INTVAL_MSEC)) {
            dtp->tkbk.intval_ms = (2000000) / flow->cfg.dtcp.bandwidth;
        } else {
            dtp->tkbk.intval_ms = TKBK_INTVAL_MSEC;
        }
        dtp->tkbk.bucket_size = (flow->cfg.dtcp.bandwidth *
                                 dtp->tkbk.intval_ms) / 8000;
        dtp->tkbk.t_last_refill = ktime_get();
    }

    return 0;
}

static struct pduft_entry *
pduft_lookup_internal(struct rl_normal *priv, rl_addr_t dst_addr)
{
    struct pduft_entry *entry;
    struct hlist_head *head;

    head = &priv->pdu_ft[hash_min(dst_addr, HASH_BITS(priv->pdu_ft))];
    hlist_for_each_entry(entry, head, node) {
        if (entry->address == dst_addr) {
            return entry;
        }
    }

    return NULL;
}

static struct flow_entry *
pduft_lookup(struct rl_normal *priv, rl_addr_t dst_addr)
{
    struct pduft_entry *entry;

    read_lock_bh(&priv->pduft_lock);
    entry = pduft_lookup_internal(priv, dst_addr);
    read_unlock_bh(&priv->pduft_lock);

    return entry ? entry->flow : NULL;
}

#define RMTQ_MAX_LEN    64

static int
rmt_tx(struct ipcp_entry *ipcp, rl_addr_t remote_addr, struct rl_buf *rb,
       bool maysleep)
{
    DECLARE_WAITQUEUE(wait, current);
    struct flow_entry *lower_flow;
    struct ipcp_entry *lower_ipcp;
    int ret;

    lower_flow = pduft_lookup((struct rl_normal *)ipcp->priv,
                              remote_addr);
    if (unlikely(!lower_flow && remote_addr != ipcp->addr)) {
        RPD(3, "No route to IPCP %lu, dropping packet\n",
            (long unsigned)remote_addr);
        rl_buf_free(rb);
        return -EHOSTUNREACH;
    }

    if (!lower_flow) {
        /* This SDU gets loopbacked to this IPCP, since this is a
         * self flow (flow->remote_addr == ipcp->addr). */
        return ipcp->ops.sdu_rx(ipcp, rb);
    }

    /* This SDU will be sent to a remote IPCP, using an N-1 flow. */

    lower_ipcp = lower_flow->txrx.ipcp;
    BUG_ON(!lower_ipcp);

    if (maysleep) {
        add_wait_queue(lower_flow->txrx.tx_wqh, &wait);
    }

    for (;;) {
        current->state = TASK_INTERRUPTIBLE;

        /* Push down to the underlying IPCP. */
        ret = lower_ipcp->ops.sdu_write(lower_ipcp, lower_flow,
                                        rb, maysleep);

        if (unlikely(ret == -EAGAIN)) {
            if (!maysleep) {
                /* Enqueue in the RMT queue, if possible. */

                spin_lock_bh(&lower_ipcp->rmtq_lock);
                if (lower_ipcp->rmtq_len < RMTQ_MAX_LEN) {
                    rb->tx_compl_flow = lower_flow;
                    list_add_tail(&rb->node, &lower_ipcp->rmtq);
                    lower_ipcp->rmtq_len++;
                } else {
                    RPD(5, "rmtq overrun: dropping PDU\n");
                    rl_buf_free(rb);
                }
                spin_unlock_bh(&lower_ipcp->rmtq_lock);

            } else {
                /* Cannot restart system call from here... */

                /* No room to write, let's sleep. */
                schedule();
                continue;
            }
        }

        break;
    }

    current->state = TASK_RUNNING;
    if (maysleep) {
        remove_wait_queue(lower_flow->txrx.tx_wqh, &wait);
    }

    return ret;
}

/* Called under DTP lock */
static int
rl_rtxq_push(struct dtp *dtp, struct rl_buf *rb)
{
    struct rl_buf *crb = rl_buf_clone(rb, GFP_ATOMIC);

    if (unlikely(!crb)) {
        PE("Out of memory\n");
        return -ENOMEM;
    }

    /* Record the rtx expiration time. */
    crb->rtx_jiffies = jiffies + dtp->rtx_tmr_int;

    /* Add to the rtx queue and start the rtx timer if not already
     * started. */
    list_add_tail(&crb->node, &dtp->rtxq);
    dtp->rtxq_len++;
    if (!timer_pending(&dtp->rtx_tmr)) {
        NPD("Forward rtx timer by %u\n",
                jiffies_to_msecs(crb->rtx_jiffies - jiffies));
        dtp->rtx_tmr_next = crb;
        mod_timer(&dtp->rtx_tmr, crb->rtx_jiffies);
    }
    NPD("cloning [%lu] into rtxq\n",
            (long unsigned)RLITE_BUF_PCI(crb)->seqnum);

    return 0;
}

static int
rl_normal_sdu_write(struct ipcp_entry *ipcp,
                    struct flow_entry *flow,
                    struct rl_buf *rb, bool maysleep)
{
    struct rina_pci *pci;
    struct dtp *dtp = &flow->dtp;
    struct fc_config *fc = &flow->cfg.dtcp.fc;
    bool dtcp_present = flow->cfg.dtcp_present;

    spin_lock_bh(&dtp->lock);

    /* Token bucket traffic shaping. */
    if (flow->cfg.dtcp.bandwidth) {
        while (dtp->tkbk.bucket_size < rb->len) {
            ktime_t now;
            unsigned long us;

            if (!maysleep) {
                spin_unlock_bh(&dtp->lock);
                return -EAGAIN;
            }

            if (timer_pending(&dtp->snd_inact_tmr)) {
                del_timer(&dtp->snd_inact_tmr);
            }

            spin_unlock_bh(&dtp->lock);
            msleep(dtp->tkbk.intval_ms);
            spin_lock_bh(&dtp->lock);

            now = ktime_get();
            us = ktime_to_us(ktime_sub(now, dtp->tkbk.t_last_refill));
            if (dtp->tkbk.bucket_size < rb->len &&
                                            us >= dtp->tkbk.intval_ms * 1000) {
                dtp->tkbk.bucket_size += ((flow->cfg.dtcp.bandwidth / 8) * us)
                                                                    / 1000000;
                dtp->tkbk.t_last_refill = now;
            }
        }
        dtp->tkbk.bucket_size -= rb->len;
    }

    if (unlikely((fc->fc_type == RLITE_FC_T_WIN &&
                 dtp->next_seq_num_to_send > dtp->snd_rwe &&
                    dtp->cwq_len >= dtp->max_cwq_len)) ||
                        (flow->cfg.dtcp.rtx_control &&
                            dtp->rtxq_len >= dtp->max_rtxq_len)) {
        /* POL: FlowControlOverrun */
        spin_unlock_bh(&dtp->lock);

        /* Backpressure. Don't drop the PDU, we will be
         * invoked again. */
        return -EAGAIN;
    }

    if (unlikely(rl_buf_pci_push(rb))) {
        flow->stats.tx_err++;
        spin_unlock_bh(&dtp->lock);
        rl_buf_free(rb);

        return -ENOSPC;
    }

    pci = RLITE_BUF_PCI(rb);
    pci->dst_addr = flow->remote_addr;
    pci->src_addr = ipcp->addr;
    pci->conn_id.qos_id = 0;
    pci->conn_id.dst_cep = flow->remote_cep;
    pci->conn_id.src_cep = flow->local_cep;
    pci->pdu_type = PDU_T_DT;
    pci->pdu_flags = 0;
    pci->pdu_len = rb->len;
    pci->seqnum = dtp->next_seq_num_to_send++;

    flow->stats.tx_pkt++;
    flow->stats.tx_byte += rb->len;

    if (unlikely(dtp->flags & DTP_F_DRF_SET)) {
        dtp->flags &= ~DTP_F_DRF_SET;
        pci->pdu_flags |= PDU_F_DRF;
    }

    if (!dtcp_present) {
        /* DTCP not present */
        dtp->snd_lwe = flow->dtp.next_seq_num_to_send; /* NIS */
        dtp->last_seq_num_sent = pci->seqnum;

    } else {
        if (fc->fc_type == RLITE_FC_T_WIN) {
            if (pci->seqnum > dtp->snd_rwe) {
                /* PDU not in the sender window, let's
                 * insert it into the Closed Window Queue.
                 * Because of the check above, we are sure
                 * that dtp->cwq_len < dtp->max_cwq_len. */
                list_add_tail(&rb->node, &dtp->cwq);
                dtp->cwq_len++;
                NPD("push [%lu] into cwq\n",
                        (long unsigned)pci->seqnum);
                rb = NULL; /* Ownership passed. */
            } else {
                /* PDU in the sender window. */
                /* POL: TxControl. */
                dtp->snd_lwe = flow->dtp.next_seq_num_to_send;
                dtp->last_seq_num_sent = pci->seqnum;
                NPD("sending [%lu] through sender window\n",
                        (long unsigned)pci->seqnum);
            }
        }

        if (rb && flow->cfg.dtcp.rtx_control) {
            int ret = rl_rtxq_push(dtp, rb);

            if (unlikely(ret)) {
                flow->stats.tx_pkt--;
                flow->stats.tx_byte -= rb->len;
                flow->stats.tx_err++;
                spin_unlock_bh(&dtp->lock);
                rl_buf_free(rb);

                return ret;
            }
        }
    }

    if (dtcp_present) {
        if (!timer_pending(&dtp->rtx_tmr)) {
            mod_timer(&dtp->snd_inact_tmr, jiffies + 3 * dtp->mpl_r_a);
        }
    }

    spin_unlock_bh(&dtp->lock);

    if (unlikely(rb == NULL)) {
        return 0;
    }

    return rmt_tx(ipcp, flow->remote_addr, rb, maysleep);
}

/* Get N-1 flow and N-1 IPCP where the mgmt PDU should be
 * written and prepare the mgmt SDU. This does not take ownership
 * of the PDU, since it's not a transmission routine. */
static int
rl_normal_mgmt_sdu_build(struct ipcp_entry *ipcp,
                           const struct rl_mgmt_hdr *mhdr,
                           struct rl_buf *rb,
                           struct ipcp_entry **lower_ipcp,
                           struct flow_entry **lower_flow)
{
    struct rl_normal *priv = (struct rl_normal *)ipcp->priv;
    struct rina_pci *pci;
    rl_addr_t dst_addr = 0; /* Not valid. */

    if (mhdr->type == RLITE_MGMT_HDR_T_OUT_DST_ADDR) {
        *lower_flow = pduft_lookup(priv, mhdr->remote_addr);
        if (unlikely(!(*lower_flow))) {
            RPD(5, "No route to IPCP %lu, dropping packet\n",
                    (long unsigned)mhdr->remote_addr);

            return -EHOSTUNREACH;
        }
        dst_addr = mhdr->remote_addr;

    } else if (mhdr->type == RLITE_MGMT_HDR_T_OUT_LOCAL_PORT) {
        *lower_flow = flow_get(mhdr->local_port);
        if (!(*lower_flow) || (*lower_flow)->upper.ipcp != ipcp) {
            RPD(5, "Invalid mgmt header local port %u, "
                    "dropping packet\n",
                    mhdr->local_port);

            if (*lower_flow) {
                flow_put(*lower_flow);
            }

            return -EINVAL;
        }
        flow_put(*lower_flow);

    } else {
        return -EINVAL;
    }
    *lower_ipcp = (*lower_flow)->txrx.ipcp;
    BUG_ON(!(*lower_ipcp));

    if (unlikely(rl_buf_pci_push(rb))) {

        return -ENOSPC;
    }

    pci = RLITE_BUF_PCI(rb);
    pci->dst_addr = dst_addr;
    pci->src_addr = ipcp->addr;
    pci->conn_id.qos_id = 0;  /* Not valid. */
    pci->conn_id.dst_cep = 0; /* Not valid. */
    pci->conn_id.src_cep = 0; /* Not valid. */
    pci->pdu_type = PDU_T_MGMT;
    pci->pdu_flags = 0; /* Not valid. */
    pci->pdu_len = rb->len;
    pci->seqnum = 0; /* Not valid. */

    /* Caller can proceed and send the mgmt PDU. */
    return 0;
}

static int
rl_normal_config(struct ipcp_entry *ipcp, const char *param_name,
                   const char *param_value)
{
    struct rl_normal *priv = (struct rl_normal *)ipcp->priv;
    int ret = -EINVAL;

    if (strcmp(param_name, "address") == 0) {
        rl_addr_t address;

        ret = kstrtou32(param_value, 10, &address);
        if (ret == 0) {
            PI("IPCP %u address set to %lu\n", ipcp->id,
               (long unsigned)address);
            ipcp->addr = address;
        }
    }

    (void)priv;

    return ret;
}

static int
rl_normal_pduft_set(struct ipcp_entry *ipcp, rl_addr_t dst_addr,
                      struct flow_entry *flow)
{
    struct rl_normal *priv = (struct rl_normal *)ipcp->priv;
    struct pduft_entry *entry;

    write_lock_bh(&priv->pduft_lock);

    entry = pduft_lookup_internal(priv, dst_addr);

    if (!entry) {
        entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
        if (!entry) {
            return -ENOMEM;
        }

        hash_add(priv->pdu_ft, &entry->node, dst_addr);
        list_add_tail(&entry->fnode, &flow->pduft_entries);
    } else {
        /* Move from the old list to the new one. */
        list_del(&entry->fnode);
        list_add_tail(&entry->fnode, &flow->pduft_entries);
    }

    entry->flow = flow;
    entry->address = dst_addr;

    write_unlock_bh(&priv->pduft_lock);

    return 0;
}

static int
rl_normal_pduft_flush(struct ipcp_entry *ipcp)
{
    struct rl_normal *priv = (struct rl_normal *)ipcp->priv;
    struct pduft_entry *entry;
    struct hlist_node *tmp;
    int bucket;

    write_lock_bh(&priv->pduft_lock);

    hash_for_each_safe(priv->pdu_ft, bucket, tmp, entry, node) {
        list_del(&entry->fnode);
        hash_del(&entry->node);
        kfree(entry);
    }

    write_unlock_bh(&priv->pduft_lock);

    return 0;
}

static int
rl_normal_pduft_del(struct ipcp_entry *ipcp, struct pduft_entry *entry)
{
    struct rl_normal *priv = (struct rl_normal *)ipcp->priv;

    write_lock_bh(&priv->pduft_lock);
    list_del(&entry->fnode);
    hash_del(&entry->node);
    write_unlock_bh(&priv->pduft_lock);

    kfree(entry);

    return 0;
}

static struct rl_buf *
ctrl_pdu_alloc(struct ipcp_entry *ipcp, struct flow_entry *flow,
                uint8_t pdu_type, rl_seq_t ack_nack_seq_num)
{
    struct rl_buf *rb = rl_buf_alloc_ctrl(ipcp->depth, GFP_ATOMIC);
    struct rina_pci_ctrl *pcic;

    if (rb) {
        pcic = (struct rina_pci_ctrl *)RLITE_BUF_DATA(rb);
        pcic->base.dst_addr = flow->remote_addr;
        pcic->base.src_addr = ipcp->addr;
        pcic->base.conn_id.qos_id = 0;
        pcic->base.conn_id.dst_cep = flow->remote_cep;
        pcic->base.conn_id.src_cep = flow->local_cep;
        pcic->base.pdu_type = pdu_type;
        pcic->base.pdu_flags = 0;
        pcic->base.pdu_len = rb->len;
        pcic->base.seqnum = flow->dtp.next_snd_ctl_seq++;
        pcic->last_ctrl_seq_num_rcvd = flow->dtp.last_ctrl_seq_num_rcvd;
        pcic->ack_nack_seq_num = ack_nack_seq_num;
        pcic->new_rwe = flow->dtp.rcv_rwe;
        pcic->new_lwe = flow->dtp.rcv_lwe;
        pcic->my_rwe = flow->dtp.snd_rwe;
        pcic->my_lwe = flow->dtp.snd_lwe;
    }

    return rb;
}

/* This must be called under DTP lock and after rcv_lwe has been
 * updated.
 */
static struct rl_buf *
sdu_rx_sv_update(struct ipcp_entry *ipcp, struct flow_entry *flow)
{
    const struct dtcp_config *cfg = &flow->cfg.dtcp;
    uint8_t pdu_type = 0;
    rl_seq_t ack_nack_seq_num = 0;

    if (cfg->flow_control) {
        /* POL: RcvrFlowControl */
        if (cfg->fc.fc_type == RLITE_FC_T_WIN) {
            NPD("rcv_rwe [%lu] --> [%lu]\n",
                    (long unsigned)flow->dtp.rcv_rwe,
                    (long unsigned)(flow->dtp.rcv_lwe +
                        flow->cfg.dtcp.fc.cfg.w.initial_credit));
            /* We should not unconditionally increment the receiver RWE,
             * but instead use some logic related to buffer management
             * (e.g. see the amount of receiver buffer available). */
            flow->dtp.rcv_rwe = flow->dtp.rcv_lwe +
                            flow->cfg.dtcp.fc.cfg.w.initial_credit;
        }
    }

    /* I know, the following code can obviously be simplified, but this
     * way policies are more visible. */
    if (cfg->rtx_control) {
        /* POL: RcvrAck */
        /* Do this here or using the A timeout ? */
        ack_nack_seq_num = flow->dtp.rcv_lwe - 1;
        pdu_type = PDU_T_CTRL_MASK | PDU_T_ACK_BIT | PDU_T_ACK;
        if (cfg->flow_control) {
            pdu_type |= PDU_T_CTRL_MASK | PDU_T_FC_BIT;
        }

    } else if (cfg->flow_control) {
        /* POL: ReceivingFlowControl */
        /* Send a flow control only control PDU. */
        pdu_type = PDU_T_CTRL_MASK | PDU_T_FC_BIT;
    }

    if (pdu_type) {
        return ctrl_pdu_alloc(ipcp, flow, pdu_type, ack_nack_seq_num);
    }

    return NULL;
}

#define SEQQ_MAX_LEN    64

/* Takes the ownership of the rb. */
static void
seqq_push(struct dtp *dtp, struct rl_buf *rb)
{
    struct rl_buf *cur;
    rl_seq_t seqnum = RLITE_BUF_PCI(rb)->seqnum;
    struct list_head *pos = &dtp->seqq;

    if (unlikely(dtp->seqq_len >= SEQQ_MAX_LEN)) {
        RPD(5, "seqq overrun: dropping PDU [%lu]\n",
                (long unsigned)seqnum);
        rl_buf_free(rb);
        return;
    }

    list_for_each_entry(cur, &dtp->seqq, node) {
        struct rina_pci *pci = RLITE_BUF_PCI(cur);

        if (seqnum < pci->seqnum) {
            pos = &cur->node;
            break;
        } else if (seqnum == pci->seqnum) {
            /* This is a duplicate amongst the gaps, we can
             * drop it. */
            rl_buf_free(rb);
            RPD(5, "Duplicate amongs the gaps [%lu] dropped\n",
                (long unsigned)seqnum);

            return;
        }
    }

    /* Insert the rb right before 'pos'. */
    list_add_tail(&rb->node, pos);
    dtp->seqq_len++;
    RPD(5, "[%lu] inserted\n", (long unsigned)seqnum);
}

static void
seqq_pop_many(struct dtp *dtp, rl_seq_t max_sdu_gap, struct list_head *qrbs)
{
    struct rl_buf *qrb, *tmp;

    INIT_LIST_HEAD(qrbs);
    list_for_each_entry_safe(qrb, tmp, &dtp->seqq, node) {
        struct rina_pci *pci = RLITE_BUF_PCI(qrb);

        if (pci->seqnum - dtp->rcv_lwe_priv <= max_sdu_gap) {
            list_del(&qrb->node);
            dtp->seqq_len--;
            list_add_tail(&qrb->node, qrbs);
            dtp->rcv_lwe_priv = pci->seqnum + 1;
            RPD(5, "[%lu] popped out from seqq\n",
                    (long unsigned)pci->seqnum);
        }
    }
}

static int
sdu_rx_ctrl(struct ipcp_entry *ipcp, struct flow_entry *flow,
            struct rl_buf *rb)
{
    struct rina_pci_ctrl *pcic = RLITE_BUF_PCI_CTRL(rb);
    struct dtp *dtp = &flow->dtp;
    struct list_head qrbs;
    struct rl_buf *qrb, *tmp;

    if (unlikely((pcic->base.pdu_type & PDU_T_CTRL_MASK)
                != PDU_T_CTRL_MASK)) {
        PE("Unknown PDU type %X\n", pcic->base.pdu_type);
        rl_buf_free(rb);
        return 0;
    }

    INIT_LIST_HEAD(&qrbs);

    spin_lock_bh(&dtp->lock);

    if (unlikely(pcic->base.seqnum > dtp->last_ctrl_seq_num_rcvd + 1)) {
        /* Gap in the control SDU space. */
        /* POL: Lost control PDU. */
        RPD(5, "Lost control PDUs: [%lu] --> [%lu]\n",
            (long unsigned)dtp->last_ctrl_seq_num_rcvd,
            (long unsigned)pcic->base.seqnum);
    } else if (unlikely(dtp->last_ctrl_seq_num_rcvd &&
                    pcic->base.seqnum <= dtp->last_ctrl_seq_num_rcvd)) {
        /* Duplicated control PDU: just drop it. */
        RPD(5, "Duplicated control PDU [%lu], last [%lu]\n",
            (long unsigned)pcic->base.seqnum,
            (long unsigned)dtp->last_ctrl_seq_num_rcvd);

        goto out;
    }

    dtp->last_ctrl_seq_num_rcvd = pcic->base.seqnum;

    if (pcic->base.pdu_type & PDU_T_FC_BIT) {
        struct rl_buf *tmp;

        if (unlikely(pcic->new_rwe < dtp->snd_rwe)) {
            /* This should not happen, the other end is
             * broken. */
            PD("Broken peer, new_rwe would go backward [%lu] "
                    "--> [%lu]\n", (long unsigned)dtp->snd_rwe,
                    (long unsigned)pcic->new_rwe);

        } else {
            NPD("snd_rwe [%lu] --> [%lu]\n",
                    (long unsigned)dtp->snd_rwe,
                    (long unsigned)pcic->new_rwe);

            /* Update snd_rwe. */
            dtp->snd_rwe = pcic->new_rwe;

            /* The update may have unblocked PDU in the cwq,
             * let's pop them out. */
            list_for_each_entry_safe(qrb, tmp, &dtp->cwq, node) {
                if (dtp->snd_lwe >= dtp->snd_rwe) {
                    break;
                }
                list_del(&qrb->node);
                dtp->cwq_len--;
                list_add_tail(&qrb->node, &qrbs);
                dtp->last_seq_num_sent = dtp->snd_lwe++;

                if (flow->cfg.dtcp.rtx_control) {
                    rl_rtxq_push(dtp, qrb);
                }

            }
        }
    }

    if (pcic->base.pdu_type & PDU_T_ACK_BIT) {
        struct rl_buf *cur, *tmp;

        switch (pcic->base.pdu_type & PDU_T_ACK_MASK) {
            case PDU_T_ACK:
                list_for_each_entry_safe(cur, tmp, &dtp->rtxq, node) {
                    struct rina_pci *pci = RLITE_BUF_PCI(cur);

                    if (pci->seqnum <= pcic->ack_nack_seq_num) {
                        NPD("Remove [%lu] from rtxq\n",
                                (long unsigned)pci->seqnum);
                        list_del(&cur->node);
                        dtp->rtxq_len--;
                        if (cur == dtp->rtx_tmr_next) {
                            /* If we acked the PDU that would have expired
                             * earliest, reset the pointer. It will be
                             * set in the else branch (if this ack does not
                             * cause the removal of all elements in the
                             * rtxq). */
                            dtp->rtx_tmr_next = NULL;
                        }
                        rl_buf_free(cur);
                    } else {
                        /* The rtxq is sorted by seqnum, so we can safely
                         * stop here. Let's update the rtx timer
                         * expiration time, if necessary. */
                        if (likely(!dtp->rtx_tmr_next)) {
                            NPD("Forward rtx timer by %u\n",
                                jiffies_to_msecs(cur->rtx_jiffies - jiffies));
                            dtp->rtx_tmr_next = cur;
                            mod_timer(&dtp->rtx_tmr, cur->rtx_jiffies);
                        }
                        break;
                    }
                }

                if (list_empty(&dtp->rtxq)) {
                    /* Everything has been acked, we can stop the rtx timer. */
                    del_timer(&dtp->rtx_tmr);
                }

                break;

            case PDU_T_NACK:
            case PDU_T_SACK:
            case PDU_T_SNACK:
                PI("Missing support for PDU type [%X]\n",
                        pcic->base.pdu_type);
                break;
        }
    }

out:
    spin_unlock_bh(&dtp->lock);

    rl_buf_free(rb);

    /* Send PDUs popped out from cwq, if any. Note that the qrbs list
     * is not emptied and must not be used after the scan.*/
    list_for_each_entry_safe(qrb, tmp, &qrbs, node) {
        struct rina_pci *pci = RLITE_BUF_PCI(qrb);

        NPD("sending [%lu] from cwq\n",
                (long unsigned)pci->seqnum);
        rmt_tx(ipcp, pci->dst_addr, qrb, false);
    }

    /* This could be done conditionally. */
    rl_write_restart_flow(flow);

    return 0;
}

static int
rl_normal_sdu_rx(struct ipcp_entry *ipcp, struct rl_buf *rb)
{
    struct rina_pci *pci = RLITE_BUF_PCI(rb);
    struct flow_entry *flow;
    rl_seq_t seqnum = pci->seqnum;
    struct rl_buf *crb = NULL;
    unsigned int a = 0;
    rl_seq_t gap;
    struct dtp *dtp;
    bool deliver;
    bool drop;
    bool qlimit;
    int ret = 0;

    if (pci->dst_addr != ipcp->addr) {
        /* The PDU is not for this IPCP, forward it. Don't propagate the
         * error code of rmt_tx(), since caller does not need it. */
        rmt_tx(ipcp, pci->dst_addr, rb, false);
        return 0;
    }

    flow = flow_get_by_cep(pci->conn_id.dst_cep);
    if (!flow) {
        RPD(5, "No flow for cep-id %u: dropping PDU\n",
                pci->conn_id.dst_cep);
        rl_buf_free(rb);
        return 0;
    }

    if (pci->pdu_type != PDU_T_DT) {
        /* This is a control PDU. */
        ret = sdu_rx_ctrl(ipcp, flow, rb);
        flow_put(flow);

        return ret;
    }

    /* This is data transfer PDU. */

    dtp = &flow->dtp;

    /* Ask rl_sdu_rx_flow() to limit the userspace queue only
     * if this flow does not use flow control. If flow control
     * is used, it will limit the userspace queue automatically. */
    qlimit = (flow->cfg.dtcp.flow_control == 0);

    spin_lock_bh(&dtp->lock);

    if (flow->cfg.dtcp_present) {
        mod_timer(&dtp->rcv_inact_tmr, jiffies + 2 * dtp->mpl_r_a);
    }

    if ((dtp->flags & DTP_F_DRF_EXPECTED) || (pci->pdu_flags & PDU_F_DRF)) {
        /* If we expect DRF being set (new PDU run) we pretend it's there
         * even if it's not int pci->pdu_flags. This is done to avoid that
         * the loss of the DRF PDU causes the loss of all the subsequent
         * packets that arrive before the transmitter realizes the DRF
         * packet was lost and can retransmit it. */

        /* Flush reassembly queue */

        dtp->flags &= ~DTP_F_DRF_EXPECTED;
        dtp->rcv_lwe = dtp->rcv_lwe_priv = seqnum + 1;
        dtp->max_seq_num_rcvd = seqnum;

        crb = sdu_rx_sv_update(ipcp, flow);

        flow->stats.rx_pkt++;
        flow->stats.rx_byte += rb->len;

        spin_unlock_bh(&dtp->lock);

        ret = rl_buf_pci_pop(rb);
        if (unlikely(ret)) {
            rl_buf_free(rb);
            goto snd_crb;
        }

        ret = rl_sdu_rx_flow(ipcp, flow, rb, qlimit);

        goto snd_crb;
    }

    if (unlikely(seqnum < dtp->rcv_lwe_priv)) {
        /* This is a duplicate. Probably we sould not drop it
         * if the flow configuration does not require it. */
        RPD(5, "Dropping duplicate PDU [seq=%lu]\n",
                (long unsigned)seqnum);
        rl_buf_free(rb);
        flow->stats.rx_err++;

        if (flow->cfg.dtcp.flow_control &&
                dtp->rcv_lwe >= dtp->last_snd_data_ack) {
            /* Send ACK flow control PDU */
            crb = ctrl_pdu_alloc(ipcp, flow, PDU_T_CTRL_MASK |
                                 PDU_T_ACK_BIT | PDU_T_ACK | PDU_T_FC_BIT,
                                 dtp->rcv_lwe);
            if (crb) {
                dtp->last_snd_data_ack = dtp->rcv_lwe;
            }
        }

        spin_unlock_bh(&dtp->lock);

        goto snd_crb;

    }

    if (unlikely(dtp->rcv_lwe_priv < seqnum &&
                seqnum <= dtp->max_seq_num_rcvd)) {
        /* This may go in a gap or be a duplicate
         * amongst the gaps. */

        NPD("Possible gap fill, RLWE_PRIV would jump %lu --> %lu\n",
                (long unsigned)dtp->rcv_lwe_priv,
                (unsigned long)seqnum + 1);

    } else if (seqnum == dtp->max_seq_num_rcvd + 1) {
        /* In order PDU. */

    } else {
        /* Out of order. */
        RPD(5, "Out of order packet, RLWE_PRIV would jump %lu --> %lu\n",
                (long unsigned)dtp->rcv_lwe_priv,
                (unsigned long)seqnum + 1);
    }

    if (seqnum > dtp->max_seq_num_rcvd) {
        dtp->max_seq_num_rcvd = seqnum;
    }

    gap = seqnum - dtp->rcv_lwe_priv;

    /* Here we may have received a PDU that it's not the next expected
     * sequence number or generally that does no meet the max_sdu_gap
     * constraint.
     * This can happen because of lost PDUs and/or out of order PDUs
     * arrival. In this case we never drop it when:
     *
     * - The flow does not require in order delivery and DTCP is
     *   not present, simply because in this case the flow is
     *   completely unreliable. Note that in this case the
     *   max_sdu_gap constraint is ignored.
     *
     * - There is RTX control, because the gaps could be filled by
     *   future retransmissions.
     *
     * - The A timeout is more than zero, because gaps could be
     *   filled by PDUs arriving out of order or retransmitted
     *   __before__ the A timer expires.
     */
    drop = ((flow->cfg.in_order_delivery || flow->cfg.dtcp_present) &&
            !a && !flow->cfg.dtcp.rtx_control &&
            gap > flow->cfg.max_sdu_gap);

    deliver = !drop && (gap <= flow->cfg.max_sdu_gap);

    if (deliver) {
        struct list_head qrbs;
        struct rl_buf *qrb, *tmp;

        /* Update rcv_lwe_priv only if this PDU is going to be
         * delivered. */
        dtp->rcv_lwe_priv = seqnum + 1;

        seqq_pop_many(dtp, flow->cfg.max_sdu_gap, &qrbs);

        if (flow->upper.ipcp) {
            dtp->rcv_lwe = dtp->rcv_lwe_priv;
            crb = sdu_rx_sv_update(ipcp, flow);
        }

        flow->stats.rx_pkt++;
        flow->stats.rx_byte += rb->len;

        spin_unlock_bh(&dtp->lock);

        ret = rl_buf_pci_pop(rb);
        if (unlikely(ret)) {
            rl_buf_free(rb);
            goto snd_crb;
        }
        ret = rl_sdu_rx_flow(ipcp, flow, rb, qlimit);

        /* Also deliver PDUs just extracted from the seqq. Note
         * that we must use the safe version of list scanning, since
         * rl_sdu_rx_flow() will modify qrb->node. */
        list_for_each_entry_safe(qrb, tmp, &qrbs, node) {
            list_del(&qrb->node);
            if (unlikely(rl_buf_pci_pop(qrb))) {
                rl_buf_free(qrb);
                continue;
            }
            ret |= rl_sdu_rx_flow(ipcp, flow, qrb, qlimit);
        }

        goto snd_crb;
    }

    if (drop) {
        RPD(5, "dropping PDU [%lu] to meet QoS requirements\n",
                (long unsigned)seqnum);
        rl_buf_free(rb);

        flow->stats.rx_err++;

    } else {
        /* What is not dropped nor delivered goes in the
         * sequencing queue.
         */
        seqq_push(dtp, rb);

        flow->stats.rx_pkt++;
        flow->stats.rx_byte += rb->len;
    }

    crb = sdu_rx_sv_update(ipcp, flow);

    spin_unlock_bh(&dtp->lock);

snd_crb:
    if (crb) {
        rmt_tx(ipcp, flow->remote_addr, crb, false);
    }

    flow_put(flow);

    return ret;
}

static int
rl_normal_sdu_rx_consumed(struct flow_entry *flow,
                            struct rl_buf *rb)
{
    struct ipcp_entry *ipcp = flow->txrx.ipcp;
    struct dtp *dtp = &flow->dtp;
    struct rl_buf *crb;

    spin_lock_bh(&dtp->lock);

    /* Update the advertised RCVLWE and send an ACK control PDU. */
    dtp->rcv_lwe = RLITE_BUF_PCI(rb)->seqnum + 1;
    crb = sdu_rx_sv_update(ipcp, flow);

    spin_unlock_bh(&dtp->lock);

    if (crb) {
        rmt_tx(ipcp, flow->remote_addr, crb, false);
    }

    return 0;
}

static int
rl_normal_flow_get_stats(struct flow_entry *flow,
                            struct rl_flow_stats *stats)
{
    struct dtp *dtp = &flow->dtp;

    spin_lock_bh(&dtp->lock);
    *stats = flow->stats;
    spin_unlock_bh(&dtp->lock);

    return 0;
}

#define SHIM_DIF_TYPE   "normal"

static struct ipcp_factory normal_factory = {
    .owner = THIS_MODULE,
    .dif_type = SHIM_DIF_TYPE,
    .create = rl_normal_create,
    .use_cep_ids = true,
    .ops.destroy = rl_normal_destroy,
    .ops.flow_allocate_req = NULL, /* Reflect to userspace. */
    .ops.flow_allocate_resp = NULL, /* Reflect to userspace. */
    .ops.flow_init = rl_normal_flow_init,
    .ops.sdu_write = rl_normal_sdu_write,
    .ops.config = rl_normal_config,
    .ops.pduft_set = rl_normal_pduft_set,
    .ops.pduft_flush = rl_normal_pduft_flush,
    .ops.pduft_del = rl_normal_pduft_del,
    .ops.mgmt_sdu_build = rl_normal_mgmt_sdu_build,
    .ops.sdu_rx = rl_normal_sdu_rx,
    .ops.flow_get_stats = rl_normal_flow_get_stats,
};

static int __init
rl_normal_init(void)
{
    return rl_ipcp_factory_register(&normal_factory);
}

static void __exit
rl_normal_fini(void)
{
    rl_ipcp_factory_unregister(SHIM_DIF_TYPE);
}

module_init(rl_normal_init);
module_exit(rl_normal_fini);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vincenzo Maffione <v.maffione@gmail.com>");
