/*
 * Control functionalities for the rlite stack.
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
#include "rlite/kernel-msg.h"
#include "rlite/utils.h"
#include "rlite-kernel.h"

#include <linux/module.h>
#include <linux/aio.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/bitmap.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>


int verbosity = RL_VERB_DBG;
EXPORT_SYMBOL(verbosity);
module_param(verbosity, int, 0644);

struct rl_ctrl;

/* The signature of a message handler. */
typedef int (*rl_msg_handler_t)(struct rl_ctrl *rc,
                                   struct rl_msg_base *bmsg);

/* Data structure associated to the /dev/rlite file descriptor. */
struct rl_ctrl {
    char msgbuf[1024];

    rl_msg_handler_t *handlers;

    /* Upqueue-related data structures. */
    struct list_head upqueue;
    unsigned int upqueue_len;
    spinlock_t upqueue_lock;
    wait_queue_head_t upqueue_wqh;

    struct list_head flows_fetch_q;
    struct list_head node;

    unsigned flags;
};

struct upqueue_entry {
    void *sermsg;
    size_t serlen;
    struct list_head node;
};

struct registered_appl {
    /* Name of the registered application. */
    struct rina_name name;

    /* The event-loop where the registered applications registered
     * (and where it can be reached by flow allocation requests). */
    struct rl_ctrl *rc;

    /* Event id used by the registration request, needed if the
     * the IPCP is partially implemented in userspace. */
    uint32_t event_id;

    /* The IPCP where the application is registered. */
    struct ipcp_entry *ipcp;

#define APPL_REG_PENDING    0x1
#define APPL_REG_COMPLETE   0x2
    /* Is registration complete or are we waiting for uipcp response? */
    uint8_t state;

    unsigned int refcnt;
    struct list_head node;
};

#define IPCP_ID_BITMAP_SIZE     1024
#define PORT_ID_BITMAP_SIZE     1024
#define CEP_ID_BITMAP_SIZE      1024
#define IPCP_HASHTABLE_BITS     7
#define PORT_ID_HASHTABLE_BITS  7
#define CEP_ID_HASHTABLE_BITS  7

struct rl_dm {
    /* Bitmap to manage IPC process ids. */
    DECLARE_BITMAP(ipcp_id_bitmap, IPCP_ID_BITMAP_SIZE);

    /* Hash table to store information about each IPC process. */
    DECLARE_HASHTABLE(ipcp_table, IPCP_HASHTABLE_BITS);

    /* Bitmap to manage port ids. */
    DECLARE_BITMAP(port_id_bitmap, PORT_ID_BITMAP_SIZE);

    /* Hash tables to store information about each flow. */
    DECLARE_HASHTABLE(flow_table, PORT_ID_HASHTABLE_BITS);
    DECLARE_HASHTABLE(flow_table_by_cep, CEP_ID_HASHTABLE_BITS);

    /* Bitmap to manage connection endpoint ids. */
    DECLARE_BITMAP(cep_id_bitmap, CEP_ID_BITMAP_SIZE);

    struct list_head ipcp_factories;

    struct list_head difs;

    /* Lock for flows table. */
    spinlock_t flows_lock;

    /* Lock for IPCPs table. */
    spinlock_t ipcps_lock;

    /* Lock for DIFs list. */
    spinlock_t difs_lock;

    /* List that contains all the rlite ctrl devices that
     * are currently opened. */
    struct list_head ctrl_devs;

    /* Lock for ipcp_factories and ctrl_devs list */
    struct mutex general_lock;

    /* Data structures for deferred removal of registered_appl structs. */
    struct list_head appl_removeq;
    struct work_struct appl_removew;
    spinlock_t appl_removeq_lock;
};

static struct rl_dm rl_dm;

#define FLOCK() spin_lock_bh(&rl_dm.flows_lock)
#define FUNLOCK() spin_unlock_bh(&rl_dm.flows_lock)
#define PLOCK() spin_lock_bh(&rl_dm.ipcps_lock)
#define PUNLOCK() spin_unlock_bh(&rl_dm.ipcps_lock)
#define RALOCK(_p) spin_lock_bh(&(_p)->regapp_lock)
#define RAUNLOCK(_p) spin_unlock_bh(&(_p)->regapp_lock)

static struct ipcp_factory *
ipcp_factories_find(const char *dif_type)
{
    struct ipcp_factory *factory;

    if (!dif_type) {
        return NULL;
    }

    list_for_each_entry(factory, &rl_dm.ipcp_factories, node) {
        if (strcmp(factory->dif_type, dif_type) == 0) {
            return factory;
        }
    }

    return NULL;
}

int
rl_ipcp_factory_register(struct ipcp_factory *factory)
{
    int ret = 0;

    if (!factory || !factory->create || !factory->owner
            || !factory->dif_type) {
        return -EINVAL;
    }

    mutex_lock(&rl_dm.general_lock);

    if (ipcp_factories_find(factory->dif_type)) {
        ret = -EBUSY;
        goto out;
    }

    /* Check if IPCP ops are ok. */
    if (!factory->ops.destroy ||
        !factory->ops.sdu_write) {
        ret = -EINVAL;
        goto out;
    }

    if (factory->ops.pduft_set && ! factory->ops.pduft_del) {
        ret = -EINVAL;
        goto out;
    }

    /* Insert the new factory into the IPC process factories
     * list. Ownership is not passed, it stills remains to
     * the invoking IPCP module. */
    list_add_tail(&factory->node, &rl_dm.ipcp_factories);

    PI("IPC processes factory '%s' registered\n",
            factory->dif_type);
out:
    mutex_unlock(&rl_dm.general_lock);

    return ret;
}
EXPORT_SYMBOL(rl_ipcp_factory_register);

int
rl_ipcp_factory_unregister(const char *dif_type)
{
    struct ipcp_factory *factory;

    mutex_lock(&rl_dm.general_lock);

    factory = ipcp_factories_find(dif_type);
    if (!factory) {
        mutex_unlock(&rl_dm.general_lock);
        return -EINVAL;
    }

    /* Just remove from the list, we don't have ownership of
     * the factory object. */
    list_del(&factory->node);

    mutex_unlock(&rl_dm.general_lock);

    PI("IPC processes factory '%s' unregistered\n",
            dif_type);

    return 0;
}
EXPORT_SYMBOL(rl_ipcp_factory_unregister);

static int
rl_upqueue_append(struct rl_ctrl *rc, const struct rl_msg_base *rmsg)
{
    struct upqueue_entry *entry;
    unsigned int serlen;
    void *serbuf;

    entry = kzalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry) {
        PE("Out of memory\n");
        return -ENOMEM;
    }

    /* Serialize the response into serbuf and then put it into the upqueue. */
    serlen = rl_msg_serlen(rl_ker_numtables, RLITE_KER_MSG_MAX, rmsg);
    serbuf = kzalloc(serlen, GFP_KERNEL);
    if (!serbuf) {
        kfree(entry);
        PE("Out of memory\n");
        return -ENOMEM;
    }
    serlen = serialize_rlite_msg(rl_ker_numtables, RLITE_KER_MSG_MAX,
                                 serbuf, rmsg);

    entry->sermsg = serbuf;
    entry->serlen = serlen;

    spin_lock(&rc->upqueue_lock);
    if (rc->upqueue_len >= 64) {
        spin_unlock(&rc->upqueue_lock);
        RPD(1, "upqueue overrun, dropping\n");
        kfree(serbuf);
        kfree(entry);
        return -ENOMEM;
    }
    list_add_tail(&entry->node, &rc->upqueue);
    rc->upqueue_len ++;
    wake_up_interruptible_poll(&rc->upqueue_wqh, POLLIN | POLLRDNORM |
                               POLLRDBAND);
    spin_unlock(&rc->upqueue_lock);

    return 0;
}

static struct dif *
dif_get(const char *dif_name, const char *dif_type, int *err)
{
    struct dif *cur;

    *err = 0;

    spin_lock_bh(&rl_dm.difs_lock);

    list_for_each_entry(cur, &rl_dm.difs, node) {
        if (strcmp(cur->name, dif_name) == 0) {
            /* A DIF called 'dif_name' already exists. */
            if (strcmp(cur->ty, dif_type) == 0) {
                cur->refcnt++;
            } else {
                /* DIF type mismatch: report error. */
                cur = NULL;
                *err = -EINVAL;
            }
            goto out;
        }
    }

    /* A DIF called 'dif_name' does not exist yet. */
    cur = kzalloc(sizeof(*cur), GFP_ATOMIC);
    if (!cur) {
        *err = -ENOMEM;
        goto out;
    }

    cur->name = kstrdup(dif_name, GFP_ATOMIC);
    if (!cur->name) {
        kfree(cur);
        cur = NULL;
        *err = -ENOMEM;
        goto out;
    }

    cur->ty = kstrdup(dif_type, GFP_ATOMIC);
    if (!cur->ty) {
        kfree(cur->name);
        kfree(cur);
        cur = NULL;
        *err = -ENOMEM;
        goto out;
    }

    cur->max_pdu_size = 8000;  /* Currently unused. */
    cur->max_pdu_life = RL_MPL_MSECS_DFLT;
    cur->refcnt = 1;
    list_add_tail(&cur->node, &rl_dm.difs);

    PD("DIF %s [type '%s'] created\n", cur->name, cur->ty);

out:
    spin_unlock_bh(&rl_dm.difs_lock);

    return cur;
}

static void
dif_put(struct dif *dif)
{
    if (!dif) {
        return;
    }

    spin_lock_bh(&rl_dm.difs_lock);
    dif->refcnt--;
    if (dif->refcnt) {
        goto out;
    }

    PD("DIF %s [type '%s'] destroyed\n", dif->name, dif->ty);

    list_del(&dif->node);
    kfree(dif->name);
    kfree(dif);

out:
    spin_unlock_bh(&rl_dm.difs_lock);
}

struct ipcp_entry *
__ipcp_get(rl_ipcp_id_t ipcp_id)
{
    struct ipcp_entry *entry;
    struct hlist_head *head;

    PLOCK();

    head = &rl_dm.ipcp_table[hash_min(ipcp_id, HASH_BITS(rl_dm.ipcp_table))];
    hlist_for_each_entry(entry, head, node) {
        if (entry->id == ipcp_id) {
            entry->refcnt++;
            PUNLOCK();
            return entry;
        }
    }

    PUNLOCK();

    return NULL;
}

struct ipcp_entry *
ipcp_select_by_dif(const char *dif_name)
{
    struct ipcp_entry *selected = NULL;
    struct ipcp_entry *entry;
    int bucket;

    PLOCK();

    /* Linear scan is not efficient, but let's stick to that for now. */
    hash_for_each(rl_dm.ipcp_table, bucket, entry, node) {
        if (!dif_name) {
            /* The request does not specify a DIF: select any DIF,
             * giving priority to normal DIFs. */
            if (!selected || (strcmp(entry->dif->ty, "normal") == 0 &&
                    (strcmp(selected->dif->ty, "normal") != 0 ||
                     entry->depth > selected->depth))) {
                selected = entry;
            }
        } else if (strcmp(entry->dif->name, dif_name) == 0) {
            selected = entry;
            break;
        }
    }

    if (selected) {
        selected->refcnt++;
    }

    PUNLOCK();

    return selected;
}

void tx_completion_func(unsigned long arg);

static int
ipcp_add_entry(struct rl_kmsg_ipcp_create *req,
               struct ipcp_entry **pentry)
{
    struct ipcp_entry *entry;
    struct ipcp_entry *cur;
    int bucket;
    struct dif *dif;
    int ret = 0;

    *pentry = NULL;

    entry = kzalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry) {
        return -ENOMEM;
    }

    PLOCK();

    /* Check if an IPC process with that name already exists.
     * This check is also performed by userspace. */
    hash_for_each(rl_dm.ipcp_table, bucket, cur, node) {
        if (rina_name_cmp(&cur->name, &req->name) == 0) {
            PUNLOCK();
            kfree(entry);
            return -EINVAL;
        }
    }

    /* Create or take a reference to the specified DIF. */
    dif = dif_get(req->dif_name, req->dif_type, &ret);
    if (!dif) {
        PUNLOCK();
        kfree(entry);
        return ret;
    }

    /* Try to alloc an IPC process id from the bitmap. */
    entry->id = bitmap_find_next_zero_area(rl_dm.ipcp_id_bitmap,
                            IPCP_ID_BITMAP_SIZE, 0, 1, 0);
    if (entry->id < IPCP_ID_BITMAP_SIZE) {
        bitmap_set(rl_dm.ipcp_id_bitmap, entry->id, 1);
        /* Build and insert an IPC process entry in the hash table. */
        rina_name_move(&entry->name, &req->name);
        entry->dif = dif;
        entry->addr = 0;
        entry->refcnt = 1;
        entry->depth = RLITE_DEFAULT_LAYERS;
        INIT_LIST_HEAD(&entry->registered_appls);
        spin_lock_init(&entry->regapp_lock);
        init_waitqueue_head(&entry->uipcp_wqh);
        mutex_init(&entry->lock);
        hash_add(rl_dm.ipcp_table, &entry->node, entry->id);
        INIT_LIST_HEAD(&entry->rmtq);
        entry->rmtq_len = 0;
        spin_lock_init(&entry->rmtq_lock);
        tasklet_init(&entry->tx_completion, tx_completion_func,
                     (unsigned long)entry);
        init_waitqueue_head(&entry->tx_wqh);
        *pentry = entry;
    } else {
        ret = -ENOSPC;
        dif_put(dif);
        kfree(entry);
    }

    PUNLOCK();

    return ret;
}

static int
ipcp_add(struct rl_kmsg_ipcp_create *req, rl_ipcp_id_t *ipcp_id)
{
    struct ipcp_factory *factory;
    struct ipcp_entry *entry = NULL;
    int ret = ipcp_add_entry(req, &entry);

    if (ret) {
        return ret;
    }

    BUG_ON(entry == NULL);

    mutex_lock(&rl_dm.general_lock);

    factory = ipcp_factories_find(req->dif_type);
    if (!factory) {
        ret = -ENXIO;
        goto out;
    }

    /* Take a reference on the module that will own the new IPC
     * process, in order to prevent the owner to be unloaded
     * while the IPC process is in use.
     * Note that this operation **must** happen before the
     * constructor invocation (factory->create()), in order to
     * avoid race conditions. */
    if (!try_module_get(factory->owner)) {
        PE("IPC process module [%s] unexpectedly "
                "disappeared\n", factory->dif_type);
        ret = -ENXIO;
        goto out;
    }
    entry->owner = factory->owner;

    entry->priv = factory->create(entry);
    if (!entry->priv) {
        ret = -EINVAL;
        goto out;
    }

    entry->ops = factory->ops;
    entry->flags |= factory->use_cep_ids ? RL_K_IPCP_USE_CEP_IDS : 0;
    *ipcp_id = entry->id;

out:
    if (ret) {
        ipcp_put(entry);
    }
    mutex_unlock(&rl_dm.general_lock);

    return ret;
}

static struct registered_appl *
__ipcp_application_get(struct ipcp_entry *ipcp,
                       const struct rina_name *appl_name)
{
    struct registered_appl *app;

    list_for_each_entry(app, &ipcp->registered_appls, node) {
        if (rina_name_cmp(&app->name, appl_name) == 0) {
            app->refcnt++;
            return app;
        }
    }

    return NULL;
}

static struct registered_appl *
ipcp_application_get(struct ipcp_entry *ipcp,
                     const struct rina_name *appl_name)
{
    struct registered_appl *app;

    RALOCK(ipcp);
    app = __ipcp_application_get(ipcp, appl_name);
    RAUNLOCK(ipcp);

    return app;
}

static void
appl_del(struct registered_appl *app)
{
    struct ipcp_entry *ipcp = app->ipcp;

    if (ipcp->ops.appl_register) {
        mutex_lock(&ipcp->lock);
        ipcp->ops.appl_register(ipcp, &app->name, 0);
        mutex_unlock(&ipcp->lock);
    }

    ipcp_put(ipcp);

    /* From here on registered application cannot be referenced anymore, and so
     * that we don't need locks. */
    rina_name_free(&app->name);
    kfree(app);
}

static void
appl_removew_func(struct work_struct *w)
{
    struct registered_appl *app, *tmp;
    struct list_head removeq;

    INIT_LIST_HEAD(&removeq);

    spin_lock_bh(&rl_dm.appl_removeq_lock);
    list_for_each_entry_safe(app, tmp, &rl_dm.appl_removeq, node) {
        list_del(&app->node);
        list_add_tail(&app->node, &removeq);
    }
    spin_unlock_bh(&rl_dm.appl_removeq_lock);

    list_for_each_entry_safe(app, tmp, &removeq, node) {
        appl_del(app);
    }
}

static void
ipcp_application_put(struct registered_appl *app)
{
    struct ipcp_entry *ipcp;

    if (!app) {
        return;
    }

    ipcp = app->ipcp;

    RALOCK(ipcp);

    app->refcnt--;
    if (app->refcnt) {
        RAUNLOCK(ipcp);
        return;
    }

    list_del(&app->node);

    RAUNLOCK(ipcp);

    if (ipcp->ops.appl_register) {
        /* Perform cleanup operation in process context, because we need
         * to take the per-ipcp mutex. */
        spin_lock_bh(&rl_dm.appl_removeq_lock);
        list_add_tail(&app->node, &rl_dm.appl_removeq);
        spin_unlock_bh(&rl_dm.appl_removeq_lock);
        schedule_work(&rl_dm.appl_removew);
    } else {
        /* No mutex required, perform the removal in current context. */
        appl_del(app);
    }
}

static int
ipcp_application_add(struct ipcp_entry *ipcp,
                     struct rina_name *appl_name,
                     struct rl_ctrl *rc,
                     uint32_t event_id,
                     bool uipcp)
{
    struct registered_appl *app, *newapp;
    int ret = 0;

    RALOCK(ipcp);
    app = __ipcp_application_get(ipcp, appl_name);
    if (app) {
        struct rl_ctrl *old_rc = app->rc;

        RAUNLOCK(ipcp);
        ipcp_application_put(app);
        if (old_rc == rc) {
            /* This registration was already asked on this
             * control device. There is nothing to do,
             * inform the caller. */
            return 1;
        }

        /* Application was already registered on a different
         * control device. */
        return -EINVAL;
    }

    /* Create a new registered application. */
    newapp = kzalloc(sizeof(*newapp), GFP_ATOMIC);
    if (!newapp) {
        return -ENOMEM;
    }

    rina_name_copy(&newapp->name, appl_name);
    newapp->rc = rc;
    newapp->event_id = event_id;
    newapp->refcnt = 1;
    newapp->ipcp = ipcp;
    newapp->state = uipcp ? APPL_REG_PENDING : APPL_REG_COMPLETE;
    list_add_tail(&newapp->node, &ipcp->registered_appls);

    RAUNLOCK(ipcp);

    PLOCK();
    ipcp->refcnt++;
    PUNLOCK();

    if (ipcp->ops.appl_register) {
        mutex_lock(&ipcp->lock);
        ret = ipcp->ops.appl_register(ipcp, appl_name, 1);
        mutex_unlock(&ipcp->lock);
        if (ret) {
            ipcp_application_put(newapp);
        }
    }

    return ret;
}

static int
ipcp_application_del(struct ipcp_entry *ipcp,
                     struct rina_name *appl_name)
{
    struct registered_appl *app;

    app = ipcp_application_get(ipcp, appl_name);
    if (!app) {
        return -EINVAL;
    }

    ipcp_application_put(app); /* To match ipcp_application_get(). */
    ipcp_application_put(app); /* To remove the application. */

    return 0;
}

static void
application_del_by_rc(struct rl_ctrl *rc)
{
    struct ipcp_entry *ipcp;
    int bucket;
    struct registered_appl *app;
    struct registered_appl *tmp;
    const char *s;
    struct list_head remove_apps;

    INIT_LIST_HEAD(&remove_apps);

    PLOCK();

    /* For each IPC processes. */
    hash_for_each(rl_dm.ipcp_table, bucket, ipcp, node) {
        RALOCK(ipcp);

        /* For each application registered to this IPC process. */
        list_for_each_entry_safe(app, tmp,
                &ipcp->registered_appls, node) {
            if (app->rc == rc) {
                if (app->refcnt == 1) {
                    /* Just move the reference. */
                    list_del(&app->node);
                    list_add_tail(&app->node, &remove_apps);
                } else {
                    /* Do what ipcp_application_put() would do, but
                     * without taking the regapp_lock. */
                    app->refcnt--;
                }
            }
        }

        RAUNLOCK(ipcp);

        /* If the control device to be deleted is an uipcp attached to
         * this IPCP, detach it. */
        if (ipcp->uipcp == rc) {
            ipcp->uipcp = NULL;
            PI("IPC process %u detached by uipcp %p\n",
                   ipcp->id, rc);
        }
    }

    PUNLOCK();

    /* Remove the selected applications without holding locks (we are in
     * process context here). */
    list_for_each_entry_safe(app, tmp, &remove_apps, node) {
        s = rina_name_to_string(&app->name);
        PD("Application %s will be automatically "
                "unregistered\n",  s);
        kfree(s);

        /* Notify userspace IPCP if needed. */
        if (app->state == APPL_REG_COMPLETE && app->ipcp->uipcp) {
            struct rl_kmsg_appl_register ntfy;

            ntfy.msg_type = RLITE_KER_APPL_REGISTER;
            ntfy.event_id = 0;
            ntfy.dif_name = app->ipcp->dif->name; /* borrow the string */
            ntfy.reg = false;
            rina_name_move(&ntfy.appl_name, &app->name);
            rl_upqueue_append(app->ipcp->uipcp,
                                (const struct rl_msg_base *)&ntfy);
            rina_name_move(&app->name, &ntfy.appl_name);
            ntfy.dif_name = NULL;  /* return the borrowed string. */
            rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX,
                        RLITE_MB(&ntfy));
        }

        /* Remove. */
        ipcp_application_put(app);
    }
}

/* To be called under FLOCK. */
struct flow_entry *
flow_lookup(rl_port_t port_id)
{
    struct flow_entry *entry;
    struct hlist_head *head;
    head = &rl_dm.flow_table[hash_min(port_id, HASH_BITS(rl_dm.flow_table))];
    hlist_for_each_entry(entry, head, node) {
        if (entry->local_port == port_id) {
            return entry;
        }
    }

    return NULL;
}
EXPORT_SYMBOL(flow_lookup);

struct flow_entry *
flow_get(rl_port_t port_id)
{
    struct flow_entry *flow;

    FLOCK();
    flow = flow_lookup(port_id);
    if (flow) {
        flow->refcnt++;
    }
    FUNLOCK();

    return flow;
}
EXPORT_SYMBOL(flow_get);

struct flow_entry *
flow_get_by_cep(unsigned int cep_id)
{
    struct flow_entry *entry;
    struct hlist_head *head;

    FLOCK();

    head = &rl_dm.flow_table_by_cep[hash_min(cep_id,
                                      HASH_BITS(rl_dm.flow_table_by_cep))];
    hlist_for_each_entry(entry, head, node_cep) {
        if (entry->local_cep == cep_id) {
            entry->refcnt++;
            FUNLOCK();
            return entry;
        }
    }

    FUNLOCK();

    return NULL;
}
EXPORT_SYMBOL(flow_get_by_cep);

void
flow_get_ref(struct flow_entry *flow)
{
    if (unlikely(!flow)) {
        return;
    }

    FLOCK();
    flow->refcnt++;
    FUNLOCK();
}
EXPORT_SYMBOL(flow_get_ref);

static struct flow_entry *
__flow_put(struct flow_entry *entry, bool maysleep)
{
    struct rl_buf *rb;
    struct rl_buf *tmp;
    struct pduft_entry *pfte, *tmp_pfte;
    struct dtp *dtp;
    struct flow_entry *ret = entry;
    struct ipcp_entry *ipcp;
    unsigned long postpone = 0;

    if (unlikely(!entry)) {
        return NULL;
    }

    FLOCK();

    dtp = &entry->dtp;

    entry->refcnt--;
    if (entry->refcnt) {
        /* Flow is still being used by someone. */
        goto out;
    }

    if (entry->cfg.dtcp_present && !maysleep) {
        /* If DTCP is present, check if we should postopone flow
         * removal. We check mauusleep to make sure
         * that this flow_entry() invocation is not due to a postponed
         * removal, so that we avoid postponing forever. */

        spin_lock_bh(&dtp->lock);
        if (dtp->cwq_len > 0 || !list_empty(&dtp->rtxq)) {
            PD("Flow removal postponed since cwq contains "
                    "%u PDUs and rtxq contains %u PDUs\n",
                    dtp->cwq_len, dtp->rtxq_len);
            postpone = 2 * HZ;

            /* No one can write or read from this flow anymore, so there
             * is no reason to have the inactivity timer running. */
            del_timer(&dtp->snd_inact_tmr);
            del_timer(&dtp->rcv_inact_tmr);
        }
        spin_unlock_bh(&dtp->lock);
    }

    if (!maysleep) {
        schedule_delayed_work(&entry->remove, postpone);
        /* Reference counter is zero here, but since the delayed
         * worker is going to use the flow, we reset the reference
         * counter to 1. The delayed worker will invoke flow_put()
         * after having performed its work.
         */
        entry->refcnt++;
        goto out;
    }

    ret = NULL;

    ipcp = entry->txrx.ipcp;

    if (ipcp->ops.flow_deallocated) {
        ipcp->ops.flow_deallocated(ipcp, entry);
    }

    if (verbosity >= RL_VERB_VERY) {
        dtp_dump(dtp);
    }
    dtp_fini(dtp);

    list_for_each_entry_safe(rb, tmp, &entry->txrx.rx_q, node) {
        list_del(&rb->node);
        rl_buf_free(rb);
    }
    entry->txrx.rx_qlen = 0;

    list_for_each_entry_safe(pfte, tmp_pfte, &entry->pduft_entries, fnode) {
        int ret;
        rl_addr_t dst_addr = pfte->address;

        BUG_ON(!entry->upper.ipcp || !entry->upper.ipcp->ops.pduft_del);
        /* Here we are sure that 'entry->upper.ipcp' will not be destroyed
         * before 'entry' is destroyed.. */
        ret = entry->upper.ipcp->ops.pduft_del(entry->upper.ipcp, pfte);
        if (ret == 0) {
            PD("Removed IPC process %u PDUFT entry: %llu --> %u\n",
                    entry->upper.ipcp->id,
                    (unsigned long long)dst_addr, entry->local_port);
        }
    }

    if (ipcp->uipcp) {
        struct rl_kmsg_flow_deallocated ntfy;

        /* Notify the uipcp about flow deallocation. */
        ntfy.msg_type = RLITE_KER_FLOW_DEALLOCATED;
        ntfy.event_id = 0;
        ntfy.ipcp_id = ipcp->id;
        ntfy.local_port_id = entry->local_port;
        ntfy.remote_port_id = entry->remote_port;
        ntfy.remote_addr = entry->remote_addr;

        rl_upqueue_append(ipcp->uipcp, (const struct rl_msg_base *)&ntfy);
        rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX,
                       RLITE_MB(&ntfy));
    }

    /* We are in process context here, so we can safely do the
     * removal. This is done for either the IPCP which supports
     * the flow (entry->txrx.ipcp) and the IPCP which uses the
     * flow (entry->upper.ipcp). */

    if (entry->upper.ipcp) {
        mutex_lock(&ipcp->lock);
        ipcp->shortcut_flows--;
        if (ipcp->shortcut_flows == 0) {
            ipcp->shortcut = NULL;
        }
        mutex_unlock(&ipcp->lock);

        ipcp_put(entry->upper.ipcp);
    }
    ipcp_put(ipcp);

    hash_del(&entry->node);
    rina_name_free(&entry->local_appl);
    rina_name_free(&entry->remote_appl);
    bitmap_clear(rl_dm.port_id_bitmap, entry->local_port, 1);
    if (ipcp->flags & RL_K_IPCP_USE_CEP_IDS) {
        hash_del(&entry->node_cep);
        bitmap_clear(rl_dm.cep_id_bitmap, entry->local_cep, 1);
    }
    PD("flow entry %u removed\n", entry->local_port);
    kfree(entry);
out:
    FUNLOCK();
    return ret;
}

struct flow_entry *
flow_put(struct flow_entry *flow)
{
    return __flow_put(flow, false);
}
EXPORT_SYMBOL(flow_put);

static void
flow_del_func(struct work_struct *work)
{
    struct flow_entry *flow = container_of(work, struct flow_entry,
                                           remove.work);
    __flow_put(flow, true);
}

static int
flow_add(struct ipcp_entry *ipcp, struct upper_ref upper,
         uint32_t event_id,
         const struct rina_name *local_appl,
         const struct rina_name *remote_appl,
         const struct rl_flow_config *flowcfg,
         struct flow_entry **pentry, gfp_t gfp)
{
    struct flow_entry *entry;
    int ret = 0;

    *pentry = entry = kzalloc(sizeof(*entry), gfp);
    if (!entry) {
        return -ENOMEM;
    }

    FLOCK();

    /* Try to alloc a port id and a cep id from the bitmaps, cep
     * ids being allocated only if needed. */
    entry->local_port = bitmap_find_next_zero_area(rl_dm.port_id_bitmap,
                                                   PORT_ID_BITMAP_SIZE,
                                                   0, 1, 0);
    if (ipcp->flags & RL_K_IPCP_USE_CEP_IDS) {
        entry->local_cep = bitmap_find_next_zero_area(rl_dm.cep_id_bitmap,
                                                      CEP_ID_BITMAP_SIZE,
                                                      0, 1, 0);
    } else {
        entry->local_cep = 0;
    }

    if (entry->local_port < PORT_ID_BITMAP_SIZE &&
                entry->local_cep < CEP_ID_BITMAP_SIZE) {
        bitmap_set(rl_dm.port_id_bitmap, entry->local_port, 1);

        if (ipcp->flags & RL_K_IPCP_USE_CEP_IDS) {
            bitmap_set(rl_dm.cep_id_bitmap, entry->local_cep, 1);
        }

        /* Build and insert a flow entry in the hash table. */
        rina_name_copy(&entry->local_appl, local_appl);
        rina_name_copy(&entry->remote_appl, remote_appl);
        entry->remote_port = 0;  /* Not valid. */
        entry->remote_cep = 0;   /* Not valid. */
        entry->remote_addr = 0;  /* Not valid. */
        entry->upper = upper;
        entry->event_id = event_id;
        entry->refcnt = 1;  /* Cogito, ergo sum. */
        entry->never_bound = true;
        INIT_LIST_HEAD(&entry->pduft_entries);
        txrx_init(&entry->txrx, ipcp, false);
        hash_add(rl_dm.flow_table, &entry->node, entry->local_port);
        if (ipcp->flags & RL_K_IPCP_USE_CEP_IDS) {
            hash_add(rl_dm.flow_table_by_cep, &entry->node_cep,
                     entry->local_cep);
        }
        INIT_DELAYED_WORK(&entry->remove, flow_del_func);
        rl_flow_stats_init(&entry->stats);
        dtp_init(&entry->dtp);
        FUNLOCK();

        PLOCK();
        ipcp->refcnt++;
        PUNLOCK();

        if (flowcfg) {
            memcpy(&entry->cfg, flowcfg, sizeof(entry->cfg));
            if (ipcp->ops.flow_init) {
                /* Let the IPCP do some
                 * specific initialization. */
                ipcp->ops.flow_init(ipcp, entry);
            }
        }
    } else {
        FUNLOCK();

        kfree(entry);
        *pentry = NULL;
        ret = -ENOSPC;
    }


    return ret;
}

static void
flow_rc_unbind(struct rl_ctrl *rc)
{
    struct flow_entry *flow;
    struct hlist_node *tmp;
    int bucket;

    hash_for_each_safe(rl_dm.flow_table, bucket, tmp, flow, node) {
        if (flow->upper.rc == rc) {
            /* Since this 'rc' is going to disappear, we have to remove
             * the reference stored into this flow. */
            flow->upper.rc = NULL;
            if (flow->txrx.state == FLOW_STATE_PENDING) {
                /* This flow is still pending. Since this rl_ctrl
                 * device is being deallocated, there won't by a way
                 * to deliver a flow allocation response, so we can
                 * remove the flow. */
                flow_put(flow);
            } else {
                /* If no rl_io device binds to this allocated flow,
                 * the associated memory will never be released.
                 * Two solutions:
                 *      (a) - When the flows transitions into allocated
                 *            state, start a timer that delete the
                 *            flow if its refcnt is still zero.
                 *      (b) - Delete the flow here if refcnt is
                 *            still zero.
                 */
            }
        }
    }
}

void
flow_make_mortal(struct flow_entry *flow)
{
    if (flow) {
        FLOCK();

        if (flow->never_bound) {
            /* Here reference counter is (likely) 2. Reset it to 1, so that
             * proper flow destruction happens in rl_io_release(). If we
             * didn't do it, the flow would live forever with its refcount
             * set to 1. */
            flow->never_bound = false;
            flow->refcnt--;
        }

        FUNLOCK();
    }
}

int
__ipcp_put(struct ipcp_entry *entry)
{
    struct rl_buf *rb, *tmp;

    if (!entry) {
        return 0;
    }

    PLOCK();

    entry->refcnt--;
    if (entry->refcnt) {
        PUNLOCK();
        return 0;
    }

    hash_del(&entry->node);
    bitmap_clear(rl_dm.ipcp_id_bitmap, entry->id, 1);

    PUNLOCK();

    /* Inoke the destructor method, if the constructor
     * was called. */
    if (entry->priv) {
        BUG_ON(entry->ops.destroy == NULL);
        /* No locking (entry->lock) is necessary here, because the current
         * thread has already removed the last reference to this IPCP,
         * and so it cannot be referenced anymore. This also means no
         * concurrent access is possible. */
        entry->ops.destroy(entry);
    }

    tasklet_kill(&entry->tx_completion);

    list_for_each_entry_safe(rb, tmp, &entry->rmtq, node) {
        list_del(&rb->node);
        rl_buf_free(rb);
    }

    /* If the module was refcounted for this IPC process instance,
     * remove the reference. Note that this operation **must** happen
     * after the destructor invokation, in order to avoid a race
     * conditions that may lead to kernel page faults. */
    if (entry->owner) {
        module_put(entry->owner);
    }

    rina_name_free(&entry->name);
    dif_put(entry->dif);

    kfree(entry);

    return 0;
}

static int
ipcp_del(rl_ipcp_id_t ipcp_id)
{
    struct ipcp_entry *entry;
    int ret = 0;

    if (ipcp_id >= IPCP_ID_BITMAP_SIZE) {
        /* No IPC process found. */
        return -ENXIO;
    }

    /* Lookup and remove the IPC process entry in the hash table corresponding
     * to the given ipcp_id. */
    entry = ipcp_get(ipcp_id);
    if (!entry) {
        return -ENXIO;
    }


    ret = ipcp_put(entry); /* To match the ipcp_get(). */

    if (entry->flags & RL_K_IPCP_ZOMBIE) {
        /* If this happens it means that someone already asked for this IPCP to
         * be destroy IPCP, so this cannot be allowed. The IPCP is still
         * referenced in the system, and will be destroyed as soon as the last
         * reference drops. */
        return -ENXIO;
    }
    entry->flags |= RL_K_IPCP_ZOMBIE;

    ret = ipcp_put(entry); /* To let the recount drop to 0. */

    return ret;
}

static int
ipcp_update_fill(struct ipcp_entry *ipcp, struct rl_kmsg_ipcp_update *upd,
                 int update_type)
{
    const char *dif_name = NULL;
    int ret = 0;

    memset(upd, 0, sizeof(*upd));

    upd->msg_type = RLITE_KER_IPCP_UPDATE;
    upd->update_type = update_type;
    upd->ipcp_id = ipcp->id;
    upd->ipcp_addr = ipcp->addr;
    upd->depth = ipcp->depth;
    if (rina_name_copy(&upd->ipcp_name, &ipcp->name)) {
        ret = -ENOMEM;
    }
    if (ipcp->dif) {
        dif_name = ipcp->dif->name;
        upd->dif_type = kstrdup(ipcp->dif->ty, GFP_ATOMIC);
        if (!upd->dif_type) {
            ret = -ENOMEM;
        }
    }
    if (dif_name) {
        upd->dif_name = kstrdup(dif_name, GFP_ATOMIC);
        if (!upd->dif_name) {
            ret = -ENOMEM;
        }
    }

    return ret;
}

static int
ipcp_update_all(rl_ipcp_id_t ipcp_id, int update_type)
{
    struct ipcp_entry *ipcp = ipcp_get(ipcp_id);
    struct rl_kmsg_ipcp_update upd;
    struct rl_ctrl *rcur;
    int ret = 0;

    if (!ipcp) {
        PE("IPCP %u unexpectedly disappeared\n", ipcp_id);
        return -ENXIO;
    }

    if (ipcp_update_fill(ipcp, &upd, update_type)) {
        PE("Out of memory\n");
        ret = -ENOMEM;
        goto out;
    }

    mutex_lock(&rl_dm.general_lock);
    list_for_each_entry(rcur, &rl_dm.ctrl_devs, node) {
        if (rcur->flags & RL_F_IPCPS) {
            rl_upqueue_append(rcur, RLITE_MB(&upd));
        }
    }
    mutex_unlock(&rl_dm.general_lock);

out:
    rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX,
                   RLITE_MB(&upd));
    ipcp_put(ipcp);

    return ret;
}

static int
rl_ipcp_create(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_ipcp_create *req = (struct rl_kmsg_ipcp_create *)bmsg;
    struct rl_kmsg_ipcp_create_resp resp;
    char *name_s = rina_name_to_string(&req->name);
    rl_ipcp_id_t ipcp_id;
    int ret;

    ret = ipcp_add(req, &ipcp_id);
    if (ret) {
        return ret;
    }

    memset(&resp, 0, sizeof(resp));
    resp.msg_type = RLITE_KER_IPCP_CREATE_RESP;
    resp.event_id = req->event_id;
    resp.ipcp_id = ipcp_id;

    /* Enqueue the response into the upqueue. */
    ret = rl_upqueue_append(rc, RLITE_MB(&resp));
    rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX,
                   RLITE_MB(&resp));
    if (ret) {
        goto err;
    }

    PI("IPC process %s created (id=%u)\n", name_s, ipcp_id);
    if (name_s) {
        kfree(name_s);
    }

    /* Upqueue an RLITE_KER_IPCP_UPDATE message to each
     * opened ctrl device. */
    ipcp_update_all(ipcp_id, RLITE_UPDATE_ADD);

    return 0;

err:
    ipcp_del(ipcp_id);

    return ret;
}

static int
rl_ipcp_destroy(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_ipcp_destroy *req =
                        (struct rl_kmsg_ipcp_destroy *)bmsg;
    int ret;

    /* Release the IPC process ID. */
    ret = ipcp_del(req->ipcp_id);

    if (ret == 0) {
        PI("IPC process %u destroyed\n", req->ipcp_id);

        {
            /* Upqueue an RLITE_KER_IPCP_UPDATE message to each
             * opened ctrl device. */
            struct rl_kmsg_ipcp_update upd;
            struct rl_ctrl *rcur;

            memset(&upd, 0, sizeof(upd));
            upd.msg_type = RLITE_KER_IPCP_UPDATE;
            upd.update_type = RLITE_UPDATE_DEL;
            upd.ipcp_id = req->ipcp_id;
            /* All the other fields are zeroed, since they are
             * not useful to userspace. */

            mutex_lock(&rl_dm.general_lock);
            list_for_each_entry(rcur, &rl_dm.ctrl_devs, node) {
                if (rcur->flags & RL_F_IPCPS) {
                    rl_upqueue_append(rcur, RLITE_MB(&upd));
                }
            }
            mutex_unlock(&rl_dm.general_lock);

            rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX,
                           RLITE_MB(&upd));
        }
    }

    return ret;
}

struct flows_fetch_q_entry {
    struct rl_kmsg_flow_fetch_resp resp;
    struct list_head node;
};

static int
rl_flow_fetch(struct rl_ctrl *rc, struct rl_msg_base *req)
{
    struct flows_fetch_q_entry *fqe;
    struct flow_entry *entry;
    int bucket;
    int ret = -ENOMEM;

    FLOCK();

    if (list_empty(&rc->flows_fetch_q)) {
        hash_for_each(rl_dm.flow_table, bucket, entry, node) {
            fqe = kmalloc(sizeof(*fqe), GFP_ATOMIC);
            if (!fqe) {
                PE("Out of memory\n");
                break;
            }

            memset(fqe, 0, sizeof(*fqe));
            list_add_tail(&fqe->node, &rc->flows_fetch_q);

            fqe->resp.msg_type = RLITE_KER_FLOW_FETCH_RESP;
            fqe->resp.end = 0;
            fqe->resp.ipcp_id = entry->txrx.ipcp->id;
            fqe->resp.local_port = entry->local_port;
            fqe->resp.remote_port = entry->remote_port;
            fqe->resp.local_addr = entry->txrx.ipcp->addr;
            fqe->resp.remote_addr = entry->remote_addr;
        }

        fqe = kmalloc(sizeof(*fqe), GFP_ATOMIC);
        if (!fqe) {
            PE("Out of memory\n");
        } else {
            memset(fqe, 0, sizeof(*fqe));
            list_add_tail(&fqe->node, &rc->flows_fetch_q);
            fqe->resp.msg_type = RLITE_KER_FLOW_FETCH_RESP;
            fqe->resp.end = 1;
        }
    }

    if (!list_empty(&rc->flows_fetch_q)) {
        fqe = list_first_entry(&rc->flows_fetch_q, struct flows_fetch_q_entry,
                               node);
        list_del(&fqe->node);
        fqe->resp.event_id = req->event_id;
        ret = rl_upqueue_append(rc, RLITE_MB(&fqe->resp));
        rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, RLITE_MB(&fqe->resp));
        kfree(fqe);
    }

    FUNLOCK();

    return ret;
}

static int
rl_ipcp_config(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_ipcp_config *req =
                    (struct rl_kmsg_ipcp_config *)bmsg;
    struct ipcp_entry *entry;
    int ret = -EINVAL;  /* Report failure by default. */

    if (!req->name || !req->value) {
        return -EINVAL;
    }

    /* Find the IPC process entry corresponding to req->ipcp_id and
     * fill the DIF name field. */
    entry = ipcp_get(req->ipcp_id);

    if (entry) {
        if (strcmp(req->name, "depth") == 0) {
            uint8_t depth;

            ret = kstrtou8(req->value, 10, &depth);
            if (ret == 0) {
                entry->depth = depth;
            }

        } else {
            mutex_lock(&entry->lock);
            if (entry->ops.config) {
                ret = entry->ops.config(entry, req->name, req->value);
            }
            mutex_unlock(&entry->lock);
        }
    }

    ipcp_put(entry);

    if (ret == 0) {
        PI("Configured IPC process %u: %s <= %s\n",
                req->ipcp_id, req->name, req->value);

        if (strcmp(req->name, "address") == 0) {
            /* Upqueue an RLITE_KER_IPCP_UPDATE message to each
             * opened ctrl device. */
            ipcp_update_all(req->ipcp_id, RLITE_UPDATE_UPD);
        }
    }

    return ret;
}

static int
rl_ipcp_pduft_set(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_ipcp_pduft_set *req =
                    (struct rl_kmsg_ipcp_pduft_set *)bmsg;
    struct ipcp_entry *ipcp;
    struct flow_entry *flow;
    int ret = -EINVAL;  /* Report failure by default. */

    flow = flow_get(req->local_port);
    ipcp = ipcp_get(req->ipcp_id);

    if (ipcp && flow && flow->upper.ipcp == ipcp && ipcp->ops.pduft_set) {
        mutex_lock(&ipcp->lock);
        /* We allow this operation only if the requesting IPCP (req->ipcp_id)
         * is really using the requested flow, i.e. 'flow->upper.ipcp == ipcp'.
         * In this situation we are sure that 'ipcp' will not be deleted before
         * 'flow' is deleted, so we can rely on the internal pduft lock. */
        ret = ipcp->ops.pduft_set(ipcp, req->dst_addr, flow);
        mutex_unlock(&ipcp->lock);
    }

    flow_put(flow);
    ipcp_put(ipcp);

    if (ret == 0) {
        PV("Set IPC process %u PDUFT entry: %llu --> %u\n",
                req->ipcp_id, (unsigned long long)req->dst_addr,
                req->local_port);
    }

    return ret;
}

static int
rl_ipcp_pduft_flush(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_ipcp_pduft_flush *req =
                    (struct rl_kmsg_ipcp_pduft_flush *)bmsg;
    struct ipcp_entry *ipcp;
    int ret = -EINVAL;  /* Report failure by default. */

    ipcp = ipcp_get(req->ipcp_id);

    if (ipcp && ipcp->ops.pduft_flush) {
        mutex_lock(&ipcp->lock);
        ret = ipcp->ops.pduft_flush(ipcp);
        mutex_unlock(&ipcp->lock);
    }

    ipcp_put(ipcp);

    if (ret == 0) {
        PV("Flushed PDUFT for IPC process %u\n", req->ipcp_id);
    }

    return ret;
}

static int
rl_ipcp_uipcp_set(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_ipcp_uipcp_set *req =
                    (struct rl_kmsg_ipcp_uipcp_set *)bmsg;
    struct ipcp_entry *entry;
    int ret = -EINVAL;  /* Report failure by default. */

    /* Find the IPC process entry corresponding to req->ipcp_id and
     * fill the entry->uipcp field. */
    entry = ipcp_get(req->ipcp_id);
    if (entry) {
        mutex_lock(&entry->lock);
        if (entry->uipcp) {
            ret = -EBUSY;
        } else {
            entry->uipcp = rc;
            ret = 0;
            wake_up_interruptible(&entry->uipcp_wqh);
        }
        mutex_unlock(&entry->lock);
    }
    ipcp_put(entry);

    if (ret == 0) {
        PI("IPC process %u attached to uipcp %p\n",
                req->ipcp_id, rc);
    }

    return ret;
}

static int
rl_ipcp_uipcp_wait(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_ipcp_uipcp_wait *req =
                    (struct rl_kmsg_ipcp_uipcp_wait *)bmsg;
    DECLARE_WAITQUEUE(wait, current);
    struct ipcp_entry *entry;
    int ret = 0;

    /* Find the IPC process entry corresponding to req->ipcp_id and wait
     * for the entry->uipcp field to be filled. */
    entry = ipcp_get(req->ipcp_id);
    if (!entry) {
        return -EINVAL;
    }

    add_wait_queue(&entry->uipcp_wqh, &wait);

    while (1) {
        struct rl_ctrl *uipcp;

        current->state = TASK_INTERRUPTIBLE;

        mutex_lock(&entry->lock);
        uipcp = entry->uipcp;
        mutex_unlock(&entry->lock);

        if (uipcp) {
            break;
        }

        if (signal_pending(current)) {
            ret = -ERESTARTSYS;
            break;
        }

        schedule();
    }

    current->state = TASK_RUNNING;
    remove_wait_queue(&entry->uipcp_wqh, &wait);

    ipcp_put(entry);

    return ret;
}

static int
rl_uipcp_fa_req_arrived(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_uipcp_fa_req_arrived *req =
                    (struct rl_kmsg_uipcp_fa_req_arrived *)bmsg;
    struct ipcp_entry *ipcp;
    int ret = -EINVAL;  /* Report failure by default. */

    ipcp = ipcp_get(req->ipcp_id);
    if (ipcp) {
        ret = rl_fa_req_arrived(ipcp, req->kevent_id, req->remote_port,
                                  req->remote_cep,
                                  req->remote_addr, &req->local_appl,
                                  &req->remote_appl, &req->flowcfg);
    }

    ipcp_put(ipcp);

    return ret;
}

static int
rl_uipcp_fa_resp_arrived(struct rl_ctrl *rc,
                           struct rl_msg_base *bmsg)
{
    struct rl_kmsg_uipcp_fa_resp_arrived *req =
                    (struct rl_kmsg_uipcp_fa_resp_arrived *)bmsg;
    struct ipcp_entry *ipcp;
    int ret = -EINVAL;  /* Report failure by default. */

    ipcp = ipcp_get(req->ipcp_id);
    if (ipcp) {
        ret = rl_fa_resp_arrived(ipcp, req->local_port, req->remote_port,
                                   req->remote_cep, req->remote_addr,
                                   req->response, &req->flowcfg);
    }
    ipcp_put(ipcp);

    return ret;
}

/* May be called under FLOCK. */
void
rl_flow_shutdown(struct flow_entry *flow)
{
    int deallocated = 0;

    spin_lock_bh(&flow->txrx.rx_lock);
    if (flow->txrx.state == FLOW_STATE_ALLOCATED) {
        /* Set the EOF condition on the flow. */
        flow->txrx.state = FLOW_STATE_DEALLOCATED;
        deallocated = 1;
    }
    spin_unlock_bh(&flow->txrx.rx_lock);

    if (deallocated) {
        /* Wake up readers and pollers, so that they can read the EOF. */
        wake_up_interruptible_poll(&flow->txrx.rx_wqh, POLLIN |
                POLLRDNORM | POLLRDBAND);
    }
}
EXPORT_SYMBOL(rl_flow_shutdown);

static int
rl_flow_dealloc(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_flow_dealloc *req =
                (struct rl_kmsg_flow_dealloc *)bmsg;
    struct flow_entry *flow;
    int ret = -ENXIO;

    flow = flow_get(req->port_id);
    if (flow) {
        rl_flow_shutdown(flow);
        ret = 0;
    }
    flow_put(flow);

    return ret;
}

static int
rl_flow_get_stats(struct rl_ctrl *rc,
                     struct rl_msg_base *bmsg)
{
    struct rl_kmsg_flow_stats_req *req =
                (struct rl_kmsg_flow_stats_req *)bmsg;
    struct rl_kmsg_flow_stats_resp resp;
    struct flow_entry *flow;
    int ret = 0;

    flow = flow_get(req->port_id);
    if (!flow) {
        return -EINVAL;
    }

    memset(&resp, 0, sizeof(resp));
    resp.msg_type = RLITE_KER_FLOW_STATS_RESP;
    resp.event_id = req->event_id;

    if (flow->txrx.ipcp->ops.flow_get_stats) {
        ret = flow->txrx.ipcp->ops.flow_get_stats(flow, &resp.stats);
    }
    flow_put(flow);

    ret = rl_upqueue_append(rc, (const struct rl_msg_base *)&resp);
    rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, RLITE_MB(&resp));

    return ret;
}

static int
rl_flow_cfg_update(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_flow_cfg_update *req =
            (struct rl_kmsg_flow_cfg_update *)bmsg;
    struct flow_entry *flow;
    int ret = 0;

    flow = flow_get(req->port_id);
    if (!flow) {
        return -EINVAL;
    }

    if (flow->txrx.ipcp->ops.flow_cfg_update) {
        ret = flow->txrx.ipcp->ops.flow_cfg_update(flow, &req->flowcfg);
    }
    flow_put(flow);

    return ret;
}

/* Connect the upper IPCP which is using this flow
 * so that rl_sdu_rx() can deliver SDU to the IPCP. */
static int
upper_ipcp_flow_bind(struct rl_ctrl *rc, rl_ipcp_id_t upper_ipcp_id,
                     struct flow_entry *flow)
{
    struct ipcp_entry *ipcp = flow->txrx.ipcp;
    struct ipcp_entry *upper_ipcp;

    /* Lookup the IPCP user of 'flow'. */
    upper_ipcp = ipcp_get(upper_ipcp_id);
    if (!upper_ipcp) {
        PE("No such upper ipcp %u\n",
                upper_ipcp_id);

        return -ENXIO;
    }

    if (upper_ipcp->uipcp != rc) {
        PE("Control device %p cannot bind flow to kernel datapath "
           "without first declaring itself an IPCP\n", rc);
        ipcp_put(upper_ipcp);

        return -EINVAL;
    }

    flow->upper.ipcp = upper_ipcp;

    mutex_lock(&ipcp->lock);
    /* The ipcp->upper_ipcp field must be set only while there is one and
     * only one upper IPCP. */
    if (ipcp->shortcut_flows == 0) {
        /* Reuse the reference, without increasing the reference counter. */
        ipcp->shortcut = upper_ipcp;
    } else if (upper_ipcp != ipcp->shortcut) {
        ipcp->shortcut = NULL;
    }
    ipcp->shortcut_flows ++;
    mutex_unlock(&ipcp->lock);

    return 0;
}

static int
rl_appl_register(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_appl_register *req =
                    (struct rl_kmsg_appl_register *)bmsg;
    struct rina_name *appl_name = &req->appl_name;
    char *name_s = rina_name_to_string(appl_name);
    struct ipcp_entry *ipcp;
    int ret = -ENXIO;  /* Report failure by default. */

    /* Find an IPC Process entry corresponding to req->dif_name. */
    ipcp = ipcp_select_by_dif(req->dif_name);
    if (ipcp) {
        ret = 0;

        if (req->reg) {
            ret = ipcp_application_add(ipcp, appl_name, rc, req->event_id,
                                       ipcp->uipcp != NULL);
        } else {
            ret = ipcp_application_del(ipcp, appl_name);
        }

        if (!ret && ipcp->uipcp) {
            /* Reflect to userspace this (un)registration, so that
             * userspace IPCP can take appropriate actions. */
            req->event_id = 0;
            rl_upqueue_append(ipcp->uipcp,
                    (const struct rl_msg_base *)req);
        }

        if (ret || !ipcp->uipcp || !req->reg) {
            /* Complete the (un)registration immediately notifying the
             * requesting application. */
            struct rl_kmsg_appl_register_resp resp;

            if (ret > 0) {
                /* ipcp_application_add() returned a positive result.
                 * This is not an error. */
                ret = 0;
            }

            resp.msg_type = RLITE_KER_APPL_REGISTER_RESP;
            resp.event_id = req->event_id;
            resp.ipcp_id = ipcp->id;
            resp.reg = req->reg;
            resp.response = ret ? RLITE_ERR : RLITE_SUCC;
            rina_name_move(&resp.appl_name, &req->appl_name);

            rl_upqueue_append(rc, (const struct rl_msg_base *)&resp);
            rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX,
                           RLITE_MB(&resp));

            if (!ret) {
                PI("Application process %s %sregistered to IPC process %u\n",
                        name_s, (req->reg ? "" : "un"), ipcp->id);
            }

            /* If ret != 0, we just appended a negative response, so the error
             * code for the system call can be reset. */
            ret = 0;
        }
    }

    ipcp_put(ipcp);

    if (name_s) {
        kfree(name_s);
    }

    return ret;
}

static int
rl_appl_register_resp(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_appl_register_resp *resp =
                    (struct rl_kmsg_appl_register_resp *)bmsg;
    struct rina_name *appl_name = &resp->appl_name;
    char *name_s = rina_name_to_string(appl_name);
    struct ipcp_entry *ipcp;
    int ret = -EINVAL;  /* Report failure by default. */

    ipcp = ipcp_get(resp->ipcp_id);

    if (!ipcp || !ipcp->uipcp || !resp->reg) {
        PE("Spurious/malicious application register response to "
           "IPCP %u\n", resp->ipcp_id);
    } else {
        struct registered_appl *app;

        app = ipcp_application_get(ipcp, &resp->appl_name);
        if (!app) {
            PE("Application register response does not match registration for "
               "'%s'\n", name_s);
        } else {
            ret = 0;
            resp->event_id = app->event_id;

            if (resp->response != 0) {
                /* Userspace IPCP denied the registration. */
                ipcp_application_put(app);

            } else {
                app->state = APPL_REG_COMPLETE;
                PI("Application process %s %sregistered to IPC process %u\n",
                        name_s, (resp->reg ? "" : "un"), resp->ipcp_id);
            }
            rl_upqueue_append(app->rc, (const struct rl_msg_base *)resp);
        }
        ipcp_application_put(app);
    }

    ipcp_put(ipcp);

    if (name_s) {
        kfree(name_s);
    }

    return ret;
}

static int
rl_append_allocate_flow_resp_arrived(struct rl_ctrl *rc, uint32_t event_id,
                                       rl_port_t port_id, uint8_t response)
{
    struct rl_kmsg_fa_resp_arrived resp;

    memset(&resp, 0, sizeof(resp));
    resp.msg_type = RLITE_KER_FA_RESP_ARRIVED;
    resp.event_id = event_id;
    resp.port_id = port_id;
    resp.response = response;

    /* Enqueue the response into the upqueue. */
    return rl_upqueue_append(rc, RLITE_MB(&resp));
}

static int
rl_fa_req(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_fa_req *req =
                    (struct rl_kmsg_fa_req *)bmsg;
    struct ipcp_entry *ipcp_entry = NULL;
    struct flow_entry *flow_entry = NULL;
    struct upper_ref upper = {
            .rc = rc,
        };
    rl_ipcp_id_t ipcp_id = -1;
    int ret = -ENXIO;

    /* Look up an IPC process entry for the specified DIF. */
    ipcp_entry = ipcp_select_by_dif(req->dif_name);
    if (!ipcp_entry) {
        goto out;
    }
    ipcp_id = ipcp_entry->id;

    /* Allocate a port id and the associated flow entry. */
    ret = flow_add(ipcp_entry, upper, req->event_id, &req->local_appl,
                   &req->remote_appl, NULL, &flow_entry,
                   GFP_KERNEL);
    if (ret) {
        goto out;
    }

    if (req->upper_ipcp_id != 0xffff) {
        ret = upper_ipcp_flow_bind(rc, req->upper_ipcp_id, flow_entry);
        if (ret) {
            goto out;
        }
    }

    if (ipcp_entry->ops.flow_allocate_req) {
        /* This IPCP handles the flow allocation in kernel-space. This is
         * currently true for shim IPCPs. */
        ret = ipcp_entry->ops.flow_allocate_req(ipcp_entry, flow_entry,
                                                &req->flowspec);
    } else {
        if (!ipcp_entry->uipcp) {
            /* No userspace IPCP to use, this happens when no uipcp is assigned
             * to this IPCP. */
            ret = -ENXIO;
        } else {
            /* This IPCP handles the flow allocation in user-space. This is
             * currently true for normal IPCPs.
             * Reflect the flow allocation request message to userspace. */
            req->event_id = 0;
            req->local_port = flow_entry->local_port;
            req->local_cep = flow_entry->local_cep;
            ret = rl_upqueue_append(ipcp_entry->uipcp,
                    (const struct rl_msg_base *)req);
        }
    }

out:
    ipcp_put(ipcp_entry);
    if (ret) {
        if (flow_entry) {
            flow_put(flow_entry);
        }

    } else {
        PD("Flow allocation requested to IPC process %u, "
               "port-id %u\n", ipcp_id, flow_entry->local_port);
    }

    if (ret == 0) {
        return 0;
    }

    /* Create a negative response message. */
    return rl_append_allocate_flow_resp_arrived(rc, req->event_id, 0, 1);
}

static int
rl_fa_resp(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_fa_resp *resp =
                    (struct rl_kmsg_fa_resp *)bmsg;
    struct flow_entry *flow_entry;
    struct ipcp_entry *ipcp;
    int ret = -EINVAL;

    /* Lookup the flow corresponding to the port-id specified
     * by the request. */
    flow_entry = flow_get(resp->port_id);
    if (!flow_entry) {
        PE("no pending flow corresponding to port-id %u\n",
                resp->port_id);
        return ret;
    }

    /* Check that the flow is in pending state and make the
     * transition to the allocated state. */
    spin_lock_bh(&flow_entry->txrx.rx_lock);
    if (flow_entry->txrx.state != FLOW_STATE_PENDING) {
        PE("flow %u is in invalid state %u\n",
                flow_entry->local_port, flow_entry->txrx.state);
        spin_unlock_bh(&flow_entry->txrx.rx_lock);
        goto out;
    }
    flow_entry->txrx.state = (resp->response == 0) ? FLOW_STATE_ALLOCATED
                                                   : FLOW_STATE_NULL;
    spin_unlock_bh(&flow_entry->txrx.rx_lock);

    PI("Flow allocation response [%u] issued to IPC process %u, "
            "port-id %u\n", resp->response, flow_entry->txrx.ipcp->id,
            flow_entry->local_port);

    if (!resp->response && resp->upper_ipcp_id != 0xffff) {
        ret = upper_ipcp_flow_bind(rc, resp->upper_ipcp_id, flow_entry);
    }

    /* Notify the involved IPC process about the response. */
    ipcp = flow_entry->txrx.ipcp;
    if (ipcp->ops.flow_allocate_resp) {
        /* This IPCP handles the flow allocation in kernel-space. This is
         * currently true for shim IPCPs. */
        ret = ipcp->ops.flow_allocate_resp(ipcp, flow_entry, resp->response);
    } else {
        if (!ipcp->uipcp) {
            /* No userspace IPCP to use, this happens when no uipcp is assigned
             * to this IPCP. */
            ret = -ENXIO;
        } else {
            /* This IPCP handles the flow allocation in user-space. This is
             * currently true for normal IPCPs.
             * Reflect the flow allocation response message to userspace. */
            resp->event_id = 0;
            resp->cep_id = flow_entry->local_cep;
            ret = rl_upqueue_append(ipcp->uipcp,
                    (const struct rl_msg_base *)resp);
        }
    }

    if (ret || resp->response) {
        flow_put(flow_entry);
    }
out:

    flow_entry = flow_put(flow_entry);

    return ret;
}

/* This may be called from softirq context. */
int
rl_fa_req_arrived(struct ipcp_entry *ipcp, uint32_t kevent_id,
                    rl_port_t remote_port, uint32_t remote_cep,
                    rl_addr_t remote_addr,
                    const struct rina_name *local_appl,
                    const struct rina_name *remote_appl,
                    const struct rl_flow_config *flowcfg)
{
    struct flow_entry *flow_entry = NULL;
    struct registered_appl *app;
    struct rl_kmsg_fa_req_arrived req;
    struct upper_ref upper;
    int ret = -EINVAL;

    /* See whether the local application is registered to this
     * IPC process. */
    app = ipcp_application_get(ipcp, local_appl);
    if (!app) {
        goto out;
    }

    /* Allocate a port id and the associated flow entry. */
    upper.rc = app->rc;
    upper.ipcp = NULL;
    ret = flow_add(ipcp, upper, 0, local_appl,
                   remote_appl, flowcfg, &flow_entry,
                   GFP_ATOMIC);
    if (ret) {
        goto out;
    }
    flow_entry->remote_port = remote_port;
    flow_entry->remote_cep = remote_cep;
    flow_entry->remote_addr = remote_addr;

    PI("Flow allocation request arrived to IPC process %u, "
        "port-id %u\n", ipcp->id, flow_entry->local_port);

    memset(&req, 0, sizeof(req));
    req.msg_type = RLITE_KER_FA_REQ_ARRIVED;
    req.event_id = 0;
    req.kevent_id = kevent_id;
    req.ipcp_id = ipcp->id;
    req.port_id = flow_entry->local_port;
    rina_name_copy(&req.local_appl, local_appl);
    rina_name_copy(&req.remote_appl, remote_appl);
    if (ipcp->dif->name) {
        req.dif_name = kstrdup(ipcp->dif->name, GFP_ATOMIC);
    }

    /* Enqueue the request into the upqueue. */
    ret = rl_upqueue_append(app->rc, RLITE_MB(&req));
    if (ret) {
        flow_put(flow_entry);
    }
    rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX,
                   RLITE_MB(&req));
out:
    ipcp_application_put(app);

    return ret;
}
EXPORT_SYMBOL(rl_fa_req_arrived);

int
rl_fa_resp_arrived(struct ipcp_entry *ipcp,
                     rl_port_t local_port,
                     rl_port_t remote_port,
                     uint32_t remote_cep,
                     rl_addr_t remote_addr,
                     uint8_t response,
                     struct rl_flow_config *flowcfg)
{
    struct flow_entry *flow_entry = NULL;
    int ret = -EINVAL;

    flow_entry = flow_get(local_port);
    if (!flow_entry) {
        return ret;
    }

    spin_lock_bh(&flow_entry->txrx.rx_lock);
    if (flow_entry->txrx.state != FLOW_STATE_PENDING) {
        spin_unlock_bh(&flow_entry->txrx.rx_lock);
        goto out;
    }
    flow_entry->txrx.state = (response == 0) ? FLOW_STATE_ALLOCATED
                                             : FLOW_STATE_NULL;
    flow_entry->remote_port = remote_port;
    flow_entry->remote_cep = remote_cep;
    flow_entry->remote_addr = remote_addr;
    spin_unlock_bh(&flow_entry->txrx.rx_lock);

    if (flowcfg) {
        memcpy(&flow_entry->cfg, flowcfg, sizeof(*flowcfg));
        if (ipcp->ops.flow_init) {
            /* Let the IPCP do some
             * specific initialization. */
            ipcp->ops.flow_init(ipcp, flow_entry);
        }
    }

    PI("Flow allocation response arrived to IPC process %u, "
            "port-id %u, remote addr %llu\n", ipcp->id,
            local_port, (long long unsigned)remote_addr);

    ret = rl_append_allocate_flow_resp_arrived(flow_entry->upper.rc,
                                                 flow_entry->event_id,
                                                 local_port, response);

    if (response) {
        /* Negative response --> delete the flow. */
        flow_put(flow_entry);
    }

out:
    flow_entry = flow_put(flow_entry);

    return ret;
}
EXPORT_SYMBOL(rl_fa_resp_arrived);

/* Share the same tx_wqh with other flows supported by the same IPCP. */
void
rl_flow_share_tx_wqh(struct flow_entry *flow)
{
    flow->txrx.tx_wqh = &flow->txrx.ipcp->tx_wqh;
}
EXPORT_SYMBOL(rl_flow_share_tx_wqh);

/* The table containing all the message handlers. */
static rl_msg_handler_t rl_ctrl_handlers[] = {
    [RLITE_KER_IPCP_CREATE] = rl_ipcp_create,
    [RLITE_KER_IPCP_DESTROY] = rl_ipcp_destroy,
    [RLITE_KER_FLOW_FETCH] = rl_flow_fetch,
    [RLITE_KER_IPCP_CONFIG] = rl_ipcp_config,
    [RLITE_KER_IPCP_PDUFT_SET] = rl_ipcp_pduft_set,
    [RLITE_KER_IPCP_PDUFT_FLUSH] = rl_ipcp_pduft_flush,
    [RLITE_KER_APPL_REGISTER] = rl_appl_register,
    [RLITE_KER_APPL_REGISTER_RESP] = rl_appl_register_resp,
    [RLITE_KER_FA_REQ] = rl_fa_req,
    [RLITE_KER_FA_RESP] = rl_fa_resp,
    [RLITE_KER_IPCP_UIPCP_SET] = rl_ipcp_uipcp_set,
    [RLITE_KER_IPCP_UIPCP_WAIT] = rl_ipcp_uipcp_wait,
    [RLITE_KER_UIPCP_FA_REQ_ARRIVED] = rl_uipcp_fa_req_arrived,
    [RLITE_KER_UIPCP_FA_RESP_ARRIVED] = rl_uipcp_fa_resp_arrived,
    [RLITE_KER_FLOW_DEALLOC] = rl_flow_dealloc,
    [RLITE_KER_FLOW_STATS_REQ] = rl_flow_get_stats,
    [RLITE_KER_FLOW_CFG_UPDATE] = rl_flow_cfg_update,
    [RLITE_KER_MSG_MAX] = NULL,
};

static ssize_t
rl_ctrl_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
    struct rl_ctrl *rc = (struct rl_ctrl *)f->private_data;
    struct rl_msg_base *bmsg;
    char *kbuf;
    ssize_t ret;

    if (len < sizeof(rl_msg_t)) {
        /* This message doesn't even contain a message type. */
        return -EINVAL;
    }

    kbuf = kmalloc(len, GFP_KERNEL);
    if (!kbuf) {
        return -ENOMEM;
    }

    /* Copy the userspace serialized message into a temporary kernelspace
     * buffer. */
    if (unlikely(copy_from_user(kbuf, ubuf, len))) {
        kfree(kbuf);
        return -EFAULT;
    }

    ret = deserialize_rlite_msg(rl_ker_numtables, RLITE_KER_MSG_MAX,
                               kbuf, len, rc->msgbuf, sizeof(rc->msgbuf));
    if (ret) {
        kfree(kbuf);
        return -EINVAL;
    }

    bmsg = RLITE_MB(rc->msgbuf);

    /* Demultiplex the message to the right message handler. */
    if (bmsg->msg_type > RLITE_KER_MSG_MAX || !rc->handlers[bmsg->msg_type]) {
        kfree(kbuf);
        return -EINVAL;
    }

    /* Check permissions. */
    switch (bmsg->msg_type) {
        case RLITE_KER_IPCP_CREATE:
        case RLITE_KER_IPCP_DESTROY:
        case RLITE_KER_IPCP_CONFIG:
        case RLITE_KER_IPCP_PDUFT_SET:
        case RLITE_KER_IPCP_PDUFT_FLUSH:
        case RLITE_KER_APPL_REGISTER_RESP:
        case RLITE_KER_IPCP_UIPCP_SET:
        case RLITE_KER_UIPCP_FA_REQ_ARRIVED:
        case RLITE_KER_UIPCP_FA_RESP_ARRIVED:
        case RLITE_KER_FLOW_DEALLOC:
        case RLITE_KER_FLOW_FETCH:
#if 0  // Skip the check for now, just to ease testing
            if (!capable(CAP_SYS_ADMIN)) {
                kfree(kbuf);
                return -EPERM;
            }
#endif
            break;
    }

    /* Carry out the requested operation. */
    ret = rc->handlers[bmsg->msg_type](rc, bmsg);
    if (ret) {
        kfree(kbuf);
        return ret;
    }

    *ppos += len;
    kfree(kbuf);

    return len;
}

static ssize_t
rl_ctrl_read(struct file *f, char __user *buf, size_t len, loff_t *ppos)
{
    DECLARE_WAITQUEUE(wait, current);
    struct upqueue_entry *entry;
    struct rl_ctrl *rc = (struct rl_ctrl *)f->private_data;
    bool blocking = !(f->f_flags & O_NONBLOCK);
    int ret = 0;

    if (blocking) {
        add_wait_queue(&rc->upqueue_wqh, &wait);
    }
    while (len) {
        current->state = TASK_INTERRUPTIBLE;

        spin_lock(&rc->upqueue_lock);
        if (list_empty(&rc->upqueue)) {
            /* No pending messages? Let's sleep. */
            spin_unlock(&rc->upqueue_lock);

            if (signal_pending(current)) {
                ret = -ERESTARTSYS;
                break;
            }

            if (!blocking) {
                ret = -EAGAIN;
                break;
            }

            schedule();
            continue;
        }

        entry = list_first_entry(&rc->upqueue, struct upqueue_entry, node);
        if (len < entry->serlen) {
            /* Not enough space? Don't pop the entry from the upqueue. */
            ret = -ENOBUFS;
        } else {
            if (unlikely(copy_to_user(buf, entry->sermsg, entry->serlen))) {
                ret = -EFAULT;
            } else {
                ret = entry->serlen;
                *ppos += ret;
            }

            /* Unlink and free the upqueue entry and the associated message. */
            list_del(&entry->node);
            rc->upqueue_len --;
            kfree(entry->sermsg);
            kfree(entry);
        }

        spin_unlock(&rc->upqueue_lock);
        break;
    }

    current->state = TASK_RUNNING;
    if (blocking) {
        remove_wait_queue(&rc->upqueue_wqh, &wait);
    }

    return ret;
}

static unsigned int
rl_ctrl_poll(struct file *f, poll_table *wait)
{
    struct rl_ctrl *rc = (struct rl_ctrl *)f->private_data;
    unsigned int mask = 0;

    poll_wait(f, &rc->upqueue_wqh, wait);

    spin_lock(&rc->upqueue_lock);
    if (!list_empty(&rc->upqueue)) {
        mask |= POLLIN | POLLRDNORM;
    }
    spin_unlock(&rc->upqueue_lock);

    mask |= POLLOUT | POLLWRNORM;

    return mask;
}

static int
initial_ipcp_update(struct rl_ctrl *rc)
{
    struct ipcp_entry *entry;
    int bucket;
    int ret = 0;

    PLOCK();

    hash_for_each(rl_dm.ipcp_table, bucket, entry, node) {
        struct rl_kmsg_ipcp_update upd;

        ret = ipcp_update_fill(entry, &upd, RLITE_UPDATE_ADD);

        rl_upqueue_append(rc, (const struct rl_msg_base *)&upd);

        rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX,
                       RLITE_MB(&upd));
    }

    PUNLOCK();

    if (ret) {
        PE("Out of memory\n");
    }

    return ret;
}

static int
rl_ctrl_open(struct inode *inode, struct file *f)
{
    struct rl_ctrl *rc;

    rc = kzalloc(sizeof(*rc), GFP_KERNEL);
    if (!rc) {
        return -ENOMEM;
    }

    f->private_data = rc;
    INIT_LIST_HEAD(&rc->upqueue);
    rc->upqueue_len = 0;
    spin_lock_init(&rc->upqueue_lock);
    init_waitqueue_head(&rc->upqueue_wqh);

    INIT_LIST_HEAD(&rc->flows_fetch_q);

    rc->handlers = rl_ctrl_handlers;

    mutex_lock(&rl_dm.general_lock);
    list_add_tail(&rc->node, &rl_dm.ctrl_devs);
    mutex_unlock(&rl_dm.general_lock);

    return 0;
}

static int
rl_ctrl_release(struct inode *inode, struct file *f)
{
    struct rl_ctrl *rc = (struct rl_ctrl *)f->private_data;

    mutex_lock(&rl_dm.general_lock);
    list_del(&rc->node);
    mutex_unlock(&rl_dm.general_lock);

    /* We must invalidate (e.g. unregister) all the
     * application names registered with this ctrl device. */
    application_del_by_rc(rc);
    flow_rc_unbind(rc);

    /* Drain upqueue. */
    {
        struct upqueue_entry *ue, *uet;

        list_for_each_entry_safe(ue, uet, &rc->upqueue, node) {
            list_del(&ue->node);
            kfree(ue->sermsg);
            kfree(ue);
        }
    }

    /* Drain flows-fetch queue. */
    {
        struct flows_fetch_q_entry *fqe, *fqet;

        list_for_each_entry_safe(fqe, fqet, &rc->flows_fetch_q, node) {
            list_del(&fqe->node);
            rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX,
                        RLITE_MB(&fqe->resp));
            kfree(fqe);
        }
    }

    kfree(rc);
    f->private_data = NULL;

    return 0;
}

static long
rl_ctrl_ioctl(struct file *f, unsigned int cmd, unsigned long flags)
{
    struct rl_ctrl *rc = (struct rl_ctrl *)f->private_data;
    unsigned int changed = flags ^ rc->flags;

    /* We have only one command, to change the flags. */
    (void) cmd;

    if (flags & ~RL_F_ALL) {
        return -EINVAL;
    }

    if (changed & flags & RL_F_IPCPS) {
        /* User turned on IPCP updates. Enqueue RLITE_KER_IPCP_UPDATE
         * messages for all the IPCPs in the system. */
        initial_ipcp_update(rc);
    }
    rc->flags = flags;

    return 0;
}

static const struct file_operations rl_ctrl_fops = {
    .owner          = THIS_MODULE,
    .release        = rl_ctrl_release,
    .open           = rl_ctrl_open,
    .write          = rl_ctrl_write,
    .read           = rl_ctrl_read,
    .poll           = rl_ctrl_poll,
    .unlocked_ioctl = rl_ctrl_ioctl,
    .llseek         = noop_llseek,
};

static struct miscdevice rl_ctrl_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "rlite",
    .fops = &rl_ctrl_fops,
};

extern struct miscdevice rl_io_misc;

static int __init
rl_ctrl_init(void)
{
    int ret;

    bitmap_zero(rl_dm.ipcp_id_bitmap, IPCP_ID_BITMAP_SIZE);
    hash_init(rl_dm.ipcp_table);
    bitmap_zero(rl_dm.port_id_bitmap, PORT_ID_BITMAP_SIZE);
    hash_init(rl_dm.flow_table);
    bitmap_zero(rl_dm.cep_id_bitmap, CEP_ID_BITMAP_SIZE);
    hash_init(rl_dm.flow_table_by_cep);
    mutex_init(&rl_dm.general_lock);
    spin_lock_init(&rl_dm.flows_lock);
    spin_lock_init(&rl_dm.ipcps_lock);
    spin_lock_init(&rl_dm.difs_lock);
    spin_lock_init(&rl_dm.appl_removeq_lock);
    INIT_LIST_HEAD(&rl_dm.ipcp_factories);
    INIT_LIST_HEAD(&rl_dm.difs);
    INIT_LIST_HEAD(&rl_dm.ctrl_devs);
    INIT_LIST_HEAD(&rl_dm.appl_removeq);
    INIT_WORK(&rl_dm.appl_removew, appl_removew_func);

    ret = misc_register(&rl_ctrl_misc);
    if (ret) {
        PE("Failed to register rlite misc device\n");
        return ret;
    }

    ret = misc_register(&rl_io_misc);
    if (ret) {
        misc_deregister(&rl_ctrl_misc);
        PE("Failed to register rlite-io misc device\n");
        return ret;
    }

    return 0;
}

static void __exit
rl_ctrl_fini(void)
{
    misc_deregister(&rl_io_misc);
    misc_deregister(&rl_ctrl_misc);
}

module_init(rl_ctrl_init);
module_exit(rl_ctrl_fini);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vincenzo Maffione <v.maffione@gmail.com>");
