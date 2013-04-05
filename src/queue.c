#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "qthread/qthread.h"
#include "qt_asserts.h"
#include "qt_debug.h"
#include "qt_threadstate.h"
#include "qt_qthread_mgmt.h"   /* for qthread_internal_self() */
#include "qt_qthread_struct.h" /* to pass data back to worker */
#include "qt_visibility.h"
#ifndef UNPOOLED
# include "qt_subsystems.h" /* for qthread_internal_cleanup() */
#endif
#include "qthread_innards.h" /* for qlib */

#include "qt_queue.h"

/* Memory Management */
#ifdef UNPOOLED
# define ALLOC_TQNODE() (qthread_queue_node_t *)MALLOC(sizeof(qthread_queue_node_t))
# define FREE_TQNODE(n) FREE((n), sizeof(qthread_queue_node_t))
void INTERNAL qthread_queue_subsystem_init(void) {}
#else
static qt_mpool node_pool = NULL;
# define ALLOC_TQNODE() (qthread_queue_node_t *)qt_mpool_alloc(node_pool)
# define FREE_TQNODE(n) qt_mpool_free(node_pool, (n))
static void qthread_queue_subsystem_shutdown(void)
{
    qt_mpool_destroy(node_pool);
}

void INTERNAL qthread_queue_subsystem_init(void)
{
    node_pool = qt_mpool_create(sizeof(qthread_queue_node_t));
    qthread_internal_cleanup(qthread_queue_subsystem_shutdown);
}

#endif /* if defined(UNPOOLED_QUEUES) || defined(UNPOOLED) */

qthread_queue_t API_FUNC qthread_queue_create(uint8_t flags,
                                              size_t  length)
{
    qthread_queue_t q = calloc(1, sizeof(struct qthread_queue_s));

    assert(q);
    if (flags & QTHREAD_QUEUE_MULTI_JOIN) {
        q->type = NEMESIS;
    } else if (flags & QTHREAD_QUEUE_MULTI_JOIN_LENGTH) {
        q->type = NEMESIS_LENGTH;
    } else if (flags & QTHREAD_QUEUE_CAPPED) {
        q->type                 = CAPPED;
        q->q.capped.maxmembers  = (aligned_t)length;
        q->q.capped.membercount = 0;
        q->q.capped.members     = MALLOC(sizeof(qthread_t *) * length);
        assert(q->q.capped.members);
    } else {
        q->type = NOSYNC;
    }
    return q;
}

int API_FUNC qthread_queue_join(qthread_queue_t q)
{
    assert(q);
    qthread_t *me = qthread_internal_self();
    me->thread_state           = QTHREAD_STATE_QUEUE;
    me->rdata->blockedon.queue = q;
    qthread_back_to_master(me);
}

void INTERNAL qthread_queue_internal_enqueue(qthread_queue_t q,
                                             qthread_t      *t)
{
    switch(q->type) {
        case NOSYNC:
            qthread_queue_internal_nosync_enqueue(&q->q.nosync, t);
            break;
        case NEMESIS:
            qthread_queue_internal_NEMESIS_enqueue(&q->q.nemesis, t);
            break;
        case NEMESIS_LENGTH:
            qthread_queue_internal_NEMESIS_enqueue(&q->q.nemesis, t);
            qthread_incr(&q->q.nemesis.length, 1);
            break;
        case CAPPED:
            qthread_queue_internal_capped_enqueue(&q->q.capped, t);
            break;
    }
}

int API_FUNC qthread_queue_release_one(qthread_queue_t q)
{
    assert(q);
    qthread_t *t;
    switch(q->type) {
        case NOSYNC:
            t = qthread_queue_internal_nosync_dequeue(&q->q.nosync);
            break;
        case NEMESIS:
            t = qthread_queue_internal_NEMESIS_dequeue(&q->q.nemesis);
            break;
        case NEMESIS_LENGTH:
            t = qthread_queue_internal_NEMESIS_dequeue(&q->q.nemesis);
            qthread_incr(&q->q.nemesis.length, -1);
            break;
        case CAPPED:
            t = qthread_queue_internal_capped_dequeue(&q->q.capped);
            break;
    }
    qthread_shepherd_id_t destination = t->target_shepherd;
#ifdef QTHREAD_USE_SPAWNCACHE
    if (destination == NO_SHEPHERD) {
        if (!qt_spawncache_spawn(t, qlib->threadqueues[destination])) {
            qt_threadqueue_enqueue(qlib->threadqueues[destination], t);
        }
    } else
#endif
    {
        qt_threadqueue_enqueue(qlib->threadqueues[destination], t);
    }
}

static void qthread_queue_internal_launch(qthread_t          *t,
                                          qthread_shepherd_t *cur_shep)
{
    assert(t);
    assert(cur_shep);
    t->thread_state = QTHREAD_STATE_RUNNING;
    if ((t->flags & QTHREAD_UNSTEALABLE) && (t->rdata->shepherd_ptr != cur_shep)) {
        qthread_debug(FEB_DETAILS, "qthread(%p:%i) enqueueing in target_shep's ready queue (%p:%i)\n", t, (int)t->thread_id, t->rdata->shepherd_ptr, (int)t->rdata->shepherd_ptr->shepherd_id);
        qt_threadqueue_enqueue(t->rdata->shepherd_ptr->ready, t);
    } else
#ifdef QTHREAD_USE_SPAWNCACHE
    if (!qt_spawncache_spawn(t, cur_shep->ready))
#endif
    {
        qthread_debug(FEB_DETAILS, "qthread(%p:%i) enqueueing in cur_shep's ready queue (%p:%i)\n", t, (int)t->thread_id, cur_shep, (int)cur_shep->shepherd_id);
        qt_threadqueue_enqueue(cur_shep->ready, t);
    }
}

int API_FUNC qthread_queue_release_all(qthread_queue_t q)
{
    assert(q);
    qthread_t          *t;
    qthread_shepherd_t *shep = qthread_internal_getshep();
    qthread_debug(FEB_DETAILS, "releasing all members of queue %p\n", q);
    switch(q->type) {
        case NOSYNC:
            while ((t = qthread_queue_internal_nosync_dequeue(&q->q.nosync)) != NULL) {
                qthread_queue_internal_launch(t, shep);
            }
            break;
        case NEMESIS:
            while ((t = qthread_queue_internal_NEMESIS_dequeue(&q->q.nemesis)) != NULL) {
                qthread_queue_internal_launch(t, shep);
            }
            break;
        case NEMESIS_LENGTH:
        {
            const aligned_t count = q->q.nemesis.length;
            for (aligned_t c = 0; c < count; c++) {
                t = qthread_queue_internal_NEMESIS_dequeue(&q->q.nemesis);
                assert(t);
                if (t) { qthread_queue_internal_launch(t, shep); }
            }
            qthread_incr(&q->q.nemesis.length, -count);
            break;
        }
        case CAPPED:
        {
            const size_t membercount  = q->q.capped.membercount;
            qthread_t  **members_copy = MALLOC(sizeof(qthread_t *) * membercount);
            assert(members_copy);
            memcpy(members_copy, q->q.capped.members, sizeof(qthread_t *) * membercount);
            memset(q->q.capped.members, 0, sizeof(qthread_t *) * membercount);
            if (membercount == q->q.capped.maxmembers) {
                q->q.capped.membercount = 0;
            }
            for (size_t c = 0; c < q->q.capped.membercount; c++) {
                if (members_copy[c] != NULL) { qthread_queue_internal_launch(members_copy[c], shep); }
            }
            break;
        }
    }
}

int API_FUNC qthread_queue_destroy(qthread_queue_t q)
{
    assert(q);
    switch(q->type) {
        case NOSYNC:
        case NEMESIS:
        case NEMESIS_LENGTH:
            break;
        case CAPPED:
            FREE(q->q.capped.members, sizeof(qthread_t *) * q->q.capped.maxmembers);
            break;
    }
    FREE(q, sizeof(struct qthread_queue_s));
    return QTHREAD_SUCCESS;
}

void INTERNAL qthread_queue_internal_nosync_enqueue(qthread_queue_nosync_t *q,
                                                    qthread_t              *t)
{
    qthread_queue_node_t *node = ALLOC_TQNODE();

    assert(node);
    assert(q);
    assert(t);

    node->thread = t;
    node->next   = NULL;
    if (q->tail == NULL) {
        q->head = node;
    } else {
        q->tail->next = node;
    }
    q->tail = node;
}

qthread_t INTERNAL *qthread_queue_internal_nosync_dequeue(qthread_queue_nosync_t *q)
{
    qthread_queue_node_t *node;
    qthread_t *t = NULL;

    assert(q);

    node = q->head;
    if (node) {
        q->head = node->next;
        t = node->thread;
        FREE_TQNODE(node);
    }
    return t;
}

void INTERNAL qthread_queue_internal_NEMESIS_enqueue(qthread_queue_NEMESIS_t *q,
                                                     qthread_t               *t)
{
    qthread_queue_node_t *node, *prev;

    node = ALLOC_TQNODE();
    assert(node != NULL);
    node->thread = t;
    node->next   = NULL;

    prev = qt_internal_atomic_swap_ptr((void **)&(q->tail), node);
    if (prev == NULL) {
        q->head = node;
    } else {
        prev->next = node;
    }
}

qthread_t INTERNAL *qthread_queue_internal_NEMESIS_dequeue(qthread_queue_NEMESIS_t *q)
{
    if (!q->shadow_head) {
        if (!q->head) {
            return NULL;
        }
        q->shadow_head = q->head;
        q->head        = NULL;
    }

    qthread_queue_node_t *const dequeued = q->shadow_head;
    if (dequeued != NULL) {
        if (dequeued->next != NULL) {
            q->shadow_head = dequeued->next;
            dequeued->next = NULL;
        } else {
            qthread_queue_node_t *old;
            q->shadow_head = NULL;
            old            = qthread_cas_ptr(&(q->tail), dequeued, NULL);
            if (old != dequeued) {
                while (dequeued->next == NULL) SPINLOCK_BODY();
                q->shadow_head = dequeued->next;
                dequeued->next = NULL;
            }
        }
    }
    qthread_t *retval = dequeued->thread;
    FREE_TQNODE(dequeued);
    return retval;
}

void INTERNAL qthread_queue_internal_capped_enqueue(qthread_queue_capped_t *q,
                                                    qthread_t              *t)
{
    abort();
}

qthread_t INTERNAL *qthread_queue_internal_capped_dequeue(qthread_queue_capped_t *q)
{
    abort();
}

/* vim:set expandtab: */