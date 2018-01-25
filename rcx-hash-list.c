#include <linux/slab.h>  // kmalloc
#include <linux/rcupdate.h>
#include <linux/types.h>


#include "hash-list.h"
#include "rtm.h"
#include "rtm_debug.h"
#include "sync_test.h"

/*
 * Abort rate rapidly grows as STATIC_PARTITION goes away from 71
 *
 * STATIC_PARTITION	Aborts per 1000 updates
 * 69			221
 * 70			171
 * 71			96
 * 72			1
 * 73			34
 * 74			101
 * 75			124
 * 76			169
 *
 * CPU topology of the system is as below:
 * [socket 0]
 * (  0, 72), (  1, 73), (  2, 74), (  3, 75), (  4, 76), (  5, 77), (  6, 78),
 * (  7, 79), (  8, 80), (  9, 81), ( 10, 82), ( 11, 83), ( 12, 84), ( 13, 85),
 * ( 14, 86), ( 15, 87), ( 16, 88), ( 17, 89),
 *
 * [socket 1]
 * ( 18, 90), ( 19, 91), ( 20, 92), ( 21, 93), ( 22, 94), ( 23, 95), ( 24, 96),
 * ( 25, 97), ( 26, 98), ( 27, 99), ( 28,100), ( 29,101), ( 30,102), ( 31,103),
 * ( 32,104), ( 33,105), ( 34,106), ( 35,107),
 *
 * [socket 2]
 * ( 36,108), ( 37,109), ( 38,110), ( 39,111), ( 40,112), ( 41,113), ( 42,114),
 * ( 43,115), ( 44,116), ( 45,117), ( 46,118), ( 47,119), ( 48,120), ( 49,121),
 * ( 50,122), ( 51,123), ( 52,124), ( 53,125),
 *
 * [socket 3]
 * ( 54,126), ( 55,127), ( 56,128), ( 57,129), ( 58,130), ( 59,131), ( 60,132),
 * ( 61,133), ( 62,134), ( 63,135), ( 64,136), ( 65,137), ( 66,138), ( 67,139),
 * ( 68,140), ( 69,141), ( 70,142), ( 71,143),
 */
#ifndef STATIC_PARTITION
#define HASH_VALUE(p_hash_list, val)    (val % p_hash_list->n_buckets)
#else
#define HASH_VALUE(p_hash_list, val)    (smp_processor_id() % STATIC_PARTITION)
#endif	/* STATIC_PARTITION */

#define RCU_READER_LOCK()               rcu_read_lock()
#define RCU_READER_UNLOCK()             rcu_read_unlock()
#define RCU_SYNCHRONIZE()               synchronize_rcu()
#define RCU_ASSIGN_PTR(p_ptr, p_obj)    rcu_assign_pointer(p_ptr, p_obj)

#define RCU_DEREF(p_obj)                (p_obj)

#define RCU_WRITER_LOCK(lock)           spin_lock(&lock)
#define RCU_WRITER_UNLOCK(lock)         spin_unlock(&lock)
#define RCU_FREE(ptr)                   kfree_rcu(ptr, rcu)

__cacheline_aligned static hash_list_t *g_hash_list;

#define pnodelock(node)	\
	(node->pnode_locks[128 * numa_node_id()])

#define pnodelockof(node, nodeid) \
	(node->pnode_locks[128 * nodeid])

#define htmlock(node) \
	(node->global_htmlock)

/*
 * Allocate a node
 */
node_t *rcx_new_node(void)
{
	int nodeid;
	node_t *p_new_node = kmalloc(sizeof(node_t), GFP_KERNEL);

	if (p_new_node == NULL)
		return NULL;

	p_new_node->removed = 0;
	p_new_node->pnode_locks[0] = 0;
	for_each_node_with_cpus(nodeid)
		pnodelockof(p_new_node, nodeid) = 0;
	p_new_node->global_lock = __SPIN_LOCK_UNLOCKED(p_new_node->global_lock);
	htmlock(p_new_node) = 0;

	return p_new_node;
}

/*
 * Free a node
 */
void rcx_free_node(node_t *p_node)
{
	RCU_FREE(p_node);
}


/**************************
 * List
 **************************/

/*
 * Allocate and initialize a list
 */
list_t *rcx_new_list(void)
{
	list_t *p_list;
	node_t *p_min_node, *p_max_node;

	p_list = kmalloc(sizeof(list_t), GFP_KERNEL);
	if (p_list == NULL)
		return NULL;

	p_max_node = rcx_new_node();
	p_max_node->val = LIST_VAL_MAX;
	p_max_node->p_next = NULL;

	p_min_node = rcx_new_node();
	p_min_node->val = LIST_VAL_MIN;
	p_min_node->p_next = p_max_node;

	p_list->p_head = p_min_node;
	p_list->rcuspin = __SPIN_LOCK_UNLOCKED(p_list->rcuspin);

	return p_list;
}

static void rcx_list_destroy(list_t *list)
{
	node_t *iter;

	for (iter = (node_t *)RCU_DEREF(list->p_head);
			iter != NULL;
			iter = iter->p_next) {
		list->p_head = iter->p_next;
		kfree(iter);
	}
}

/*
 * Get number of entries in given list
 */
static int list_size(list_t *p_list)
{
	int size = 0;
	node_t *p_node;

	/* We have at least 2 elements */
	p_node = p_list->p_head->p_next;
	while (p_node->p_next != NULL) {
		size++;
		p_node = p_node->p_next;
	}

	return size;
}

/*
 * Check whether give value is in the given list
 *
 * Returns one if exists, zero else
 */
int rcx_list_contains(list_t *p_list, val_t val)
{
	int result;
	val_t v;
	node_t *p_prev, *p_next;
	node_t *p_node;

	RCU_READER_LOCK();

	p_prev = (node_t *)RCU_DEREF(p_list->p_head);
	p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	while (1) {
		p_node = (node_t *)RCU_DEREF(p_next);
		v = p_node->val;

		if (v >= val)
			break;

		p_prev = p_next;
		p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	}

	result = (v == val);

	RCU_READER_UNLOCK();

	return result;
}

/*
 * Insert a value into a list
 *
 * Returns two for aborts, one if the value is in the list already, or zero if
 * insert done and success.
 */
int rcx_list_add(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	val_t v;
	int tx_stat;

	p_prev = (node_t *)RCU_DEREF(p_list->p_head);
	p_next = (node_t *)RCU_DEREF(p_prev->p_next);

	while (1) {
		p_node = (node_t *)RCU_DEREF(p_next);
		v = p_node->val;

		if (v >= val)
			break;

		p_prev = p_next;
		p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	}

	result = (v != val);

	if (result) {
		node_t *p_new_node = rcx_new_node();

		p_new_node->val = val;
		p_new_node->p_next = p_next;

		tx_stat = _xbegin();
		if (tx_stat == _XBEGIN_STARTED) {
			if (RCU_DEREF(p_prev->p_next) != p_next)
				_xabort(ABORT_CONFLICT);
			if (p_prev->removed || p_next->removed)
				_xabort(ABORT_DOUBLE_FREE);

			RCU_ASSIGN_PTR((p_prev->p_next), p_new_node);
			_xend();
		} else {
			record_abort(tx_stat);
			kfree(p_new_node);
			return 2;
		}
	}

	return result;
}

#define LF_RETRY_LIMIT	10

/*
 * Insert a value into a list using locking fallback
 *
 * Returns one if the value is in the list already, or zero if insert done and
 * success.
 */
int rcx_list_lf_add(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	val_t v;
	int tx_stat;
	int retries = 0;

retry:
	if (retries++ > LF_RETRY_LIMIT) {
		retries = 0;
		RCU_WRITER_LOCK(p_list->rcuspin);

		p_prev = (node_t *)RCU_DEREF(p_list->p_head);
		p_next = (node_t *)RCU_DEREF(p_prev->p_next);

		while (1) {
			p_node = (node_t *)RCU_DEREF(p_next);
			v = p_node->val;

			if (v >= val)
				break;

			p_prev = p_next;
			p_next = (node_t *)RCU_DEREF(p_prev->p_next);
		}

		result = (v != val);

		if (result) {
			node_t *p_new_node = rcx_new_node();

			p_new_node->val = val;
			p_new_node->p_next = p_next;

			RCU_ASSIGN_PTR((p_prev->p_next), p_new_node);
		}
		RCU_WRITER_UNLOCK(p_list->rcuspin);

		return result;
	}

	p_prev = (node_t *)RCU_DEREF(p_list->p_head);
	p_next = (node_t *)RCU_DEREF(p_prev->p_next);

	while (1) {
		p_node = (node_t *)RCU_DEREF(p_next);
		v = p_node->val;

		if (v >= val)
			break;

		p_prev = p_next;
		p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	}

	result = (v != val);

	if (result) {
		node_t *p_new_node = rcx_new_node();

		p_new_node->val = val;
		p_new_node->p_next = p_next;

		while (spin_is_locked(&p_list->rcuspin))
			;
		tx_stat = _xbegin();
		if (tx_stat == _XBEGIN_STARTED) {
			if (spin_is_locked(&p_list->rcuspin))
				_xabort(ABORT_LF_CONFLICT);
			if (RCU_DEREF(p_prev->p_next) != p_next)
				_xabort(ABORT_CONFLICT);
			if (p_prev->removed || p_next->removed)
				_xabort(ABORT_DOUBLE_FREE);

			RCU_ASSIGN_PTR((p_prev->p_next), p_new_node);
			_xend();
		} else {
			record_abort(tx_stat);
			kfree(p_new_node);
			goto retry;
		}
	}

	return result;
}

/*
 * Insert a value into a list using abort reason and locking fallback
 *
 * Returns one if the value is in the list already, or zero if insert done and
 * success.
 */
int rcx_list_fb1_add(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	val_t v;
	int tx_stat;

htm_path:
	p_prev = (node_t *)RCU_DEREF(p_list->p_head);
	p_next = (node_t *)RCU_DEREF(p_prev->p_next);

	while (1) {
		p_node = (node_t *)RCU_DEREF(p_next);
		v = p_node->val;

		if (v >= val)
			break;

		p_prev = p_next;
		p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	}

	result = (v != val);

	if (result) {
		node_t *p_new_node = rcx_new_node();

		p_new_node->val = val;
		p_new_node->p_next = p_next;

		while (spin_is_locked(&p_list->rcuspin))
			;
		tx_stat = _xbegin();
		if (tx_stat == _XBEGIN_STARTED) {
			if (spin_is_locked(&p_list->rcuspin))
				_xabort(ABORT_LF_CONFLICT);
			if (RCU_DEREF(p_prev->p_next) != p_next)
				_xabort(ABORT_CONFLICT);
			if (p_prev->removed || p_next->removed)
				_xabort(ABORT_DOUBLE_FREE);

			RCU_ASSIGN_PTR((p_prev->p_next), p_new_node);
			_xend();
		} else {
			record_abort(tx_stat);
			kfree(p_new_node);
			if (tx_stat & _XABORT_RETRY)
				goto htm_path;
			else
				goto locking_path;
		}
	}

	return result;

locking_path:
	RCU_WRITER_LOCK(p_list->rcuspin);

	p_prev = (node_t *)RCU_DEREF(p_list->p_head);
	p_next = (node_t *)RCU_DEREF(p_prev->p_next);

	while (1) {
		p_node = (node_t *)RCU_DEREF(p_next);
		v = p_node->val;

		if (v >= val)
			break;

		p_prev = p_next;
		p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	}

	result = (v != val);

	if (result) {
		node_t *p_new_node = rcx_new_node();

		p_new_node->val = val;
		p_new_node->p_next = p_next;

		RCU_ASSIGN_PTR((p_prev->p_next), p_new_node);
	}
	RCU_WRITER_UNLOCK(p_list->rcuspin);

	return result;
}

/*
 * Insert a value into a list protected by the only HTM based locking
 *
 * Same with fine-grained rcu, but use HTM for locking.
 */
int rcx_list_htmlock_add(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	val_t v;
	int tx_stat;

retry:
	p_prev = (node_t *)RCU_DEREF(p_list->p_head);
	p_next = (node_t *)RCU_DEREF(p_prev->p_next);

	while (1) {
		p_node = (node_t *)RCU_DEREF(p_next);
		v = p_node->val;

		if (v >= val)
			break;

		p_prev = p_next;
		p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	}

	result = (v != val);

	if (result) {
		node_t *p_new_node = rcx_new_node();

		p_new_node->val = val;
		p_new_node->p_next = p_next;

		while (htmlock(p_prev) == 1 || htmlock(p_next) == 1)
			smp_wmb();

		tx_stat = _xbegin();
		if (tx_stat == _XBEGIN_STARTED) {
			if (htmlock(p_prev) == 1 || htmlock(p_next) == 1)
				_xabort(ABORT_CONFLICT);

			htmlock(p_prev) = 1;
			htmlock(p_next) = 1;
			_xend();
		} else {
			record_abort(tx_stat);
			kfree(p_new_node);
			goto retry;
		}

		/*
		 * Now there are no concurrent updaters, though previous
		 * updaters could already touched something.
		 */
		if (RCU_DEREF(p_prev->p_next) != p_next) {
			record_abort(ABORT_CONFLICT);
			goto unlock_retry;
		}
		if (p_prev->removed || p_next->removed) {
			record_abort(ABORT_DOUBLE_FREE);
			goto unlock_retry;
		}
		RCU_ASSIGN_PTR((p_prev->p_next), p_new_node);
		htmlock(p_next) = 0;
		htmlock(p_prev) = 0;
		return result;

unlock_retry:
		htmlock(p_next) = 0;
		htmlock(p_prev) = 0;
		kfree(p_new_node);
		goto retry;
	}

	return result;
}

/*
 * Insert a value into a list protected by the only HTM based hierarchical
 * locking
 *
 * Same with fine-grained rcu, but use HTM for locking.
 */
int rcx_list_hhtmlock_add(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	val_t v;
	int tx_stat;

retry:
	p_prev = (node_t *)RCU_DEREF(p_list->p_head);
	p_next = (node_t *)RCU_DEREF(p_prev->p_next);

	while (1) {
		p_node = (node_t *)RCU_DEREF(p_next);
		v = p_node->val;

		if (v >= val)
			break;

		p_prev = p_next;
		p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	}

	result = (v != val);

	if (result) {
		node_t *p_new_node = rcx_new_node();

		p_new_node->val = val;
		p_new_node->p_next = p_next;

		while (pnodelock(p_prev) == 1 || pnodelock(p_next) == 1)
			smp_wmb();

		tx_stat = _xbegin();
		if (tx_stat == _XBEGIN_STARTED) {
			/* HTM CS.  It touches per-node locks only.  Slim
			 * enough, no many contention */
			if (pnodelock(p_prev) == 1 || pnodelock(p_next) == 1)
				_xabort(ABORT_CONFLICT);

			pnodelock(p_prev) = 1;
			pnodelock(p_next) = 1;
			_xend();
		} else {
			record_abort(tx_stat);
			kfree(p_new_node);
			goto retry;
		}

retry_global_lock:
		while (htmlock(p_prev) == 1 || htmlock(p_next) == 1)
			smp_wmb();

		tx_stat = _xbegin();
		if (tx_stat == _XBEGIN_STARTED) {
			if (htmlock(p_prev) == 1 || htmlock(p_next) == 1)
				_xabort(ABORT_CONFLICT);

			htmlock(p_prev) = 1;
			htmlock(p_next) = 1;
			_xend();
		} else {
			record_abort(tx_stat);
			goto retry_global_lock;
		}

		/*
		 * Now there are no concurrent updaters, though previous
		 * updaters could already touched something.
		 */
		if (RCU_DEREF(p_prev->p_next) != p_next) {
			record_abort(ABORT_CONFLICT);
			goto unlock_retry;
		}
		if (p_prev->removed || p_next->removed) {
			record_abort(ABORT_DOUBLE_FREE);
			goto unlock_retry;
		}
		RCU_ASSIGN_PTR((p_prev->p_next), p_new_node);
		htmlock(p_prev) = 0;
		htmlock(p_next) = 0;
		pnodelock(p_prev) = 0;
		pnodelock(p_next) = 0;
		return result;

unlock_retry:
		htmlock(p_prev) = 0;
		htmlock(p_next) = 0;
		pnodelock(p_next) = 0;
		pnodelock(p_prev) = 0;
		kfree(p_new_node);
		goto retry;
	}

	return result;
}

/*
 * Insert a value into a list in NUMA-awared manner
 *
 * Returns one if the value is in the list already, or zero if insert done and
 * success.
 */
int rcx_list_numa_add(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	val_t v;
	int tx_stat;

retry:
	p_prev = (node_t *)RCU_DEREF(p_list->p_head);
	p_next = (node_t *)RCU_DEREF(p_prev->p_next);

	while (1) {
		p_node = (node_t *)RCU_DEREF(p_next);
		v = p_node->val;

		if (v >= val)
			break;

		p_prev = p_next;
		p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	}

	result = (v != val);

	if (result) {
		node_t *p_new_node = rcx_new_node();

		p_new_node->val = val;
		p_new_node->p_next = p_next;

		while (pnodelock(p_prev) == 1 || pnodelock(p_next) == 1)
			;

		tx_stat = _xbegin();
		if (tx_stat == _XBEGIN_STARTED) {
			/* HTM CS.  It touches per-node locks only.  Slim
			 * enough, no many contention */
			if (pnodelock(p_prev) == 1 || pnodelock(p_next) == 1)
				_xabort(ABORT_CONFLICT);

			pnodelock(p_prev) = 1;
			pnodelock(p_next) = 1;
			_xend();
		} else {
			record_abort(tx_stat);
			kfree(p_new_node);
			goto retry;
		}

		RCU_WRITER_LOCK(p_prev->global_lock);
		RCU_WRITER_LOCK(p_next->global_lock);
		/*
		 * Spinlock CS.  Now there is no concurrent updaters, though
		 * previous updaters could already touched something.
		 */
		if (RCU_DEREF(p_prev->p_next) != p_next) {
			record_abort(ABORT_CONFLICT);
			goto unlock_retry;
		}
		if (p_prev->removed || p_next->removed) {
			record_abort(ABORT_DOUBLE_FREE);
			goto unlock_retry;
		}
		RCU_ASSIGN_PTR((p_prev->p_next), p_new_node);
		RCU_WRITER_UNLOCK(p_next->global_lock);
		RCU_WRITER_UNLOCK(p_prev->global_lock);
		pnodelock(p_prev) = 0;
		pnodelock(p_next) = 0;
		return result;

unlock_retry:
		RCU_WRITER_UNLOCK(p_next->global_lock);
		RCU_WRITER_UNLOCK(p_prev->global_lock);
		pnodelock(p_next) = 0;
		pnodelock(p_prev) = 0;
		kfree(p_new_node);
		goto retry;
	}

	return result;
}

/*
 * Deletes a value from a list
 *
 * Returns 2 if aborted, 1 if success, 0 if the list doesn't contain the value.
 */
int rcx_list_remove(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	node_t *n;
	int tx_stat;

	p_prev = (node_t *)RCU_DEREF(p_list->p_head);
	p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	while (1) {
		p_node = (node_t *)RCU_DEREF(p_next);

		if (p_node->val >= val)
			break;

		p_prev = p_next;
		p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	}

	result = (p_node->val == val);

	if (result) {
		n = (node_t *)RCU_DEREF(p_next->p_next);
		tx_stat = _xbegin();
		if (tx_stat == _XBEGIN_STARTED) {
			if (p_prev->removed || p_next->removed || n->removed)
				_xabort(ABORT_DOUBLE_FREE);
			if (RCU_DEREF(p_prev->p_next) != p_next ||
					RCU_DEREF(p_next->p_next) != n)
				_xabort(ABORT_CONFLICT);
			RCU_ASSIGN_PTR((p_prev->p_next), n);
			p_next->removed = 1;
			_xend();
		} else {
			record_abort(tx_stat);
			return 2;
		}

		rcx_free_node(p_next);

		return result;
	}

	return result;
}

/*
 * Deletes a value from a list with locking fallback
 *
 * Returns 1 if success, 0 if the list doesn't contain the value.
 */
int rcx_list_lf_remove(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	node_t *n;
	int tx_stat;
	int retries = 0;

retry:

	if (retries++ >= LF_RETRY_LIMIT) {
		retries = 0;
		RCU_WRITER_LOCK(p_list->rcuspin);

		p_prev = (node_t *)RCU_DEREF(p_list->p_head);
		p_next = (node_t *)RCU_DEREF(p_prev->p_next);
		while (1) {
			p_node = (node_t *)RCU_DEREF(p_next);

			if (p_node->val >= val)
				break;

			p_prev = p_next;
			p_next = (node_t *)RCU_DEREF(p_prev->p_next);
		}

		result = (p_node->val == val);
		if (result) {
			n = (node_t *)RCU_DEREF(p_next->p_next);
			RCU_ASSIGN_PTR((p_prev->p_next), n);
			n->removed = 1;
			RCU_WRITER_UNLOCK(p_list->rcuspin);
			rcx_free_node(p_next);

			return 1;
		}

		RCU_WRITER_UNLOCK(p_list->rcuspin);
		return 0;
	}

	p_prev = (node_t *)RCU_DEREF(p_list->p_head);
	p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	while (1) {
		p_node = (node_t *)RCU_DEREF(p_next);

		if (p_node->val >= val)
			break;

		p_prev = p_next;
		p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	}

	result = (p_node->val == val);

	if (result) {
		while (spin_is_locked(&p_list->rcuspin))
			;
		n = (node_t *)RCU_DEREF(p_next->p_next);
		tx_stat = _xbegin();
		if (tx_stat == _XBEGIN_STARTED) {
			if (spin_is_locked(&p_list->rcuspin))
				_xabort(ABORT_LF_CONFLICT);
			if (p_prev->removed || p_next->removed || n->removed)
				_xabort(ABORT_DOUBLE_FREE);
			if (RCU_DEREF(p_prev->p_next) != p_next ||
					RCU_DEREF(p_next->p_next) != n)
				_xabort(ABORT_CONFLICT);
			RCU_ASSIGN_PTR((p_prev->p_next), n);
			p_next->removed = 1;
			_xend();
		} else {
			record_abort(tx_stat);
			goto retry;
		}

		rcx_free_node(p_next);

		return result;
	}

	return result;
}

/*
 * Deletes a value from a list with locking fallback, retry based on abort
 * reason
 *
 * Returns 1 if success, 0 if the list doesn't contain the value.
 */
int rcx_list_fb1_remove(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	node_t *n;
	int tx_stat;

htm_path:
	p_prev = (node_t *)RCU_DEREF(p_list->p_head);
	p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	while (1) {
		p_node = (node_t *)RCU_DEREF(p_next);

		if (p_node->val >= val)
			break;

		p_prev = p_next;
		p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	}

	result = (p_node->val == val);

	if (result) {
		while (spin_is_locked(&p_list->rcuspin))
			;
		n = (node_t *)RCU_DEREF(p_next->p_next);
		tx_stat = _xbegin();
		if (tx_stat == _XBEGIN_STARTED) {
			if (spin_is_locked(&p_list->rcuspin))
				_xabort(ABORT_LF_CONFLICT);
			if (p_prev->removed || p_next->removed || n->removed)
				_xabort(ABORT_DOUBLE_FREE);
			if (RCU_DEREF(p_prev->p_next) != p_next ||
					RCU_DEREF(p_next->p_next) != n)
				_xabort(ABORT_CONFLICT);
			RCU_ASSIGN_PTR((p_prev->p_next), n);
			p_next->removed = 1;
			_xend();
		} else {
			record_abort(tx_stat);
			if (tx_stat & _XABORT_RETRY)
				goto htm_path;
			else
				goto locking_path;
		}

		rcx_free_node(p_next);

		return result;
	}

	return result;

locking_path:
	RCU_WRITER_LOCK(p_list->rcuspin);

	p_prev = (node_t *)RCU_DEREF(p_list->p_head);
	p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	while (1) {
		p_node = (node_t *)RCU_DEREF(p_next);

		if (p_node->val >= val)
			break;

		p_prev = p_next;
		p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	}

	result = (p_node->val == val);
	if (result) {
		n = (node_t *)RCU_DEREF(p_next->p_next);
		RCU_ASSIGN_PTR((p_prev->p_next), n);
		n->removed = 1;
		RCU_WRITER_UNLOCK(p_list->rcuspin);
		rcx_free_node(p_next);

		return 1;
	}

	RCU_WRITER_UNLOCK(p_list->rcuspin);
	return 0;
}

/*
 * Deletes a value from a list in NUMA-awared manner, using HTM lock
 *
 * Returns 1 if success, 0 if the list doesn't contain the value.
 */
int rcx_list_htmlock_remove(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	node_t *n;
	int tx_stat;

retry:
	p_prev = (node_t *)RCU_DEREF(p_list->p_head);
	p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	while (1) {
		p_node = (node_t *)RCU_DEREF(p_next);

		if (p_node->val >= val)
			break;

		p_prev = p_next;
		p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	}

	result = (p_node->val == val);

	if (result) {
		n = (node_t *)RCU_DEREF(p_next->p_next);
		/* p_prev -> p_next -> n */

		while (htmlock(p_prev) == 1 || htmlock(p_next) == 1 ||
				htmlock(n) == 1)
			smp_wmb();

		tx_stat = _xbegin();
		if (tx_stat == _XBEGIN_STARTED) {
			if (htmlock(p_prev) == 1 || htmlock(p_next) == 1 ||
					htmlock(n) == 1)
				_xabort(ABORT_CONFLICT);

			htmlock(p_prev) = 1;
			htmlock(p_next) = 1;
			htmlock(n) = 1;
			_xend();
		} else {
			record_abort(tx_stat);
			goto retry;
		}

		/* Complete CS. */
		if (p_prev->removed || p_next->removed || n->removed) {
			record_abort(ABORT_DOUBLE_FREE);
			goto unlock_retry;
		}
		if (RCU_DEREF(p_prev->p_next) != p_next ||
				RCU_DEREF(p_next->p_next) != n) {
			record_abort(ABORT_CONFLICT);
			goto unlock_retry;
		}

		RCU_ASSIGN_PTR((p_prev->p_next), n);
		p_next->removed = 1;
		rcx_free_node(p_next);

		htmlock(n) = 0;
		htmlock(p_next) = 0;
		htmlock(p_prev) = 0;
		return result;

unlock_retry:
		htmlock(n) = 0;
		htmlock(p_next) = 0;
		htmlock(p_prev) = 0;
		goto retry;
	}

	return result;
}

/*
 * Deletes a value from a list in NUMA-awared manner, using HTM
 *
 * Returns 1 if success, 0 if the list doesn't contain the value.
 */
int rcx_list_hhtmlock_remove(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	node_t *n;
	int tx_stat;

retry:
	p_prev = (node_t *)RCU_DEREF(p_list->p_head);
	p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	while (1) {
		p_node = (node_t *)RCU_DEREF(p_next);

		if (p_node->val >= val)
			break;

		p_prev = p_next;
		p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	}

	result = (p_node->val == val);

	if (result) {
		n = (node_t *)RCU_DEREF(p_next->p_next);
		/* p_prev -> p_next -> n */

		while (pnodelock(p_prev) == 1 || pnodelock(p_next) == 1 ||
				pnodelock(n) == 1)
			smp_wmb();

		tx_stat = _xbegin();
		if (tx_stat == _XBEGIN_STARTED) {
			if (pnodelock(p_prev) == 1 || pnodelock(p_next) == 1 ||
					pnodelock(n) == 1)
				_xabort(ABORT_CONFLICT);

			pnodelock(p_prev) = 1;
			pnodelock(p_next) = 1;
			pnodelock(n) = 1;
			_xend();
		} else {
			record_abort(tx_stat);
			goto retry;
		}

retry_global_lock:
		while (htmlock(p_prev) == 1 || htmlock(p_next) == 1 ||
				htmlock(n) == 1)
			smp_wmb();

		tx_stat = _xbegin();
		if (tx_stat == _XBEGIN_STARTED) {
			if (htmlock(p_prev) == 1 || htmlock(p_next) == 1 ||
					htmlock(n) == 1)
				_xabort(ABORT_CONFLICT);
			htmlock(p_prev) = 1;
			htmlock(p_next) = 1;
			htmlock(n) = 1;
			_xend();
		} else {
			record_abort(tx_stat);
			goto retry_global_lock;
		}

		/* Complete CS. */
		if (p_prev->removed || p_next->removed || n->removed) {
			record_abort(ABORT_DOUBLE_FREE);
			goto unlock_retry;
		}
		if (RCU_DEREF(p_prev->p_next) != p_next ||
				RCU_DEREF(p_next->p_next) != n) {
			record_abort(ABORT_CONFLICT);
			goto unlock_retry;
		}

		RCU_ASSIGN_PTR((p_prev->p_next), n);
		p_next->removed = 1;
		rcx_free_node(p_next);

		htmlock(n) = 0;
		htmlock(p_next) = 0;
		htmlock(p_prev) = 0;
		pnodelock(n) = 0;
		pnodelock(p_next) = 0;
		pnodelock(p_prev) = 0;

		return result;


unlock_retry:

		htmlock(n) = 0;
		htmlock(p_next) = 0;
		htmlock(p_prev) = 0;
		pnodelock(n) = 0;
		pnodelock(p_next) = 0;
		pnodelock(p_prev) = 0;
		goto retry;
	}

	return result;
}

/*
 * Deletes a value from a list in NUMA-awared manner
 *
 * Returns 1 if success, 0 if the list doesn't contain the value.
 */
int rcx_list_numa_remove(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	node_t *n;
	int tx_stat;

retry:
	p_prev = (node_t *)RCU_DEREF(p_list->p_head);
	p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	while (1) {
		p_node = (node_t *)RCU_DEREF(p_next);

		if (p_node->val >= val)
			break;

		p_prev = p_next;
		p_next = (node_t *)RCU_DEREF(p_prev->p_next);
	}

	result = (p_node->val == val);

	if (result) {
		n = (node_t *)RCU_DEREF(p_next->p_next);
		/* p_prev -> p_next -> n */

		while (pnodelock(p_prev) == 1 || pnodelock(p_next) == 1 ||
				pnodelock(n) == 1)
			;

		tx_stat = _xbegin();
		if (tx_stat == _XBEGIN_STARTED) {
			if (pnodelock(p_prev) == 1 || pnodelock(p_next) == 1 ||
					pnodelock(n) == 1)
				_xabort(ABORT_CONFLICT);

			pnodelock(p_prev) = 1;
			pnodelock(p_next) = 1;
			pnodelock(n) = 1;

			/*
			if (p_prev->removed || p_next->removed || n->removed)
				_xabort(ABORT_DOUBLE_FREE);
			if (RCU_DEREF(p_prev->p_next) != p_next ||
					RCU_DEREF(p_next->p_next) != n)
				_xabort(ABORT_CONFLICT);
			RCU_ASSIGN_PTR((p_prev->p_next), n);
			p_next->removed = 1;
			*/
			_xend();
		} else {
			record_abort(tx_stat);
			goto retry;
		}

		RCU_WRITER_LOCK(p_prev->global_lock);
		RCU_WRITER_LOCK(p_next->global_lock);
		RCU_WRITER_LOCK(n->global_lock);

		/* Spinlock CS. */
		if (p_prev->removed || p_next->removed || n->removed) {
			record_abort(ABORT_DOUBLE_FREE);
			goto unlock_retry;
		}
		if (RCU_DEREF(p_prev->p_next) != p_next ||
				RCU_DEREF(p_next->p_next) != n) {
			record_abort(ABORT_CONFLICT);
			goto unlock_retry;
		}

		RCU_ASSIGN_PTR((p_prev->p_next), n);
		p_next->removed = 1;
		rcx_free_node(p_next);

		RCU_WRITER_UNLOCK(n->global_lock);
		RCU_WRITER_UNLOCK(p_next->global_lock);
		RCU_WRITER_UNLOCK(p_prev->global_lock);
		pnodelock(n) = 0;
		pnodelock(p_next) = 0;
		pnodelock(p_prev) = 0;

		return result;


unlock_retry:

		RCU_WRITER_UNLOCK(n->global_lock);
		RCU_WRITER_UNLOCK(p_next->global_lock);
		RCU_WRITER_UNLOCK(p_prev->global_lock);
		pnodelock(n) = 0;
		pnodelock(p_next) = 0;
		pnodelock(p_prev) = 0;
		goto retry;
	}

	return result;
}


/**************************
 * Hash List
 **************************/

/*
 * Allocate and initialize a hash list
 */
hash_list_t *rcx_new_hash_list(int n_buckets)
{
	int i;
	hash_list_t *p_hash_list;

	p_hash_list = kmalloc(sizeof(hash_list_t), GFP_KERNEL);

	if (p_hash_list == NULL)
		return NULL;

	p_hash_list->n_buckets = n_buckets;

	for (i = 0; i < p_hash_list->n_buckets; i++)
		p_hash_list->buckets[i] = rcx_new_list();

	return p_hash_list;
}

/*
 * Setup the global hash list
 */
int rcx_hash_list_init(int nr_buckets, void *dat)
{
	g_hash_list = rcx_new_hash_list(nr_buckets);
	return 0;
}

/*
 * Destroy a hash list.
 *
 * Caller of this function should guarantee that there is no other concurrent
 * threads accessing the hash list.
 */
void rcx_hash_list_destroy(void)
{
	int hash;

	for (hash = 0; hash < g_hash_list->n_buckets; hash++) {
		rcx_list_destroy(g_hash_list->buckets[hash]);
		/* This is why the name is destroy, not empty */
		kfree(g_hash_list->buckets[hash]);
	}
	kfree(g_hash_list);
}

/*
 * Get number of entries in given hash list
 */
__attribute__ ((unused))
static int hash_list_size(hash_list_t *p_hash_list)
{
	int i;
	int size = 0;

	for (i = 0; i < p_hash_list->n_buckets; i++)
		size += list_size(p_hash_list->buckets[i]);

	return size;
}

/*
 * Check whether a value is in the global hash list
 *
 * Returns zero if exists, -ENOENT else
 */
int rcx_hash_list_contains(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	return rcx_list_contains(g_hash_list->buckets[hash], val) ?
		0 : -ENOENT;
}

/*
 * Inserts a value into the global hash list
 *
 * This function should have a fallback mechanism for abort in future.
 *
 * Returns -1 if aborted, zero else
 */
int rcx_hash_list_add(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);
	int result = rcx_list_add(g_hash_list->buckets[hash], val);

	if (result == 2)
		/* abort! */
		return -1;
	else
		return 0;
}

/*
 * Try-and-fail version of rcx_hash_list_add()
 *
 * In detail, there is no any difference with rcx_hash_list_add().
 *
 * Returns -1 if aborted, zero else
 */
int rcx_hash_list_try_add(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);
	int result = rcx_list_add(g_hash_list->buckets[hash], val);

	if (result == 2)
		/* abort! */
		return -1;
	else
		return 0;
}

/*
 * Retry version of rcx_hash_list_try_add()
 *
 * If it aborts, it retries until success but return iirescoverable abort if it
 * cannot success until the benchmark time finished.
 *
 * Returns 0 for success, -1 for irrecoverable abort
 */
int rcx_hash_list_retry_add(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);
	int result;

	for (;;) {
		result = rcx_list_add(g_hash_list->buckets[hash], val);

		if (result != 2)
			break;
		if (benchmark_endtime() != 1)
			continue;
		return -1;
	}
	return 0;
}

/*
 * Locking fallback version of rcx_hash_list_add()
 *
 * Returns zero only
 */
int rcx_hash_list_lf_add(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	rcx_list_lf_add(g_hash_list->buckets[hash], val);
	return 0;
}

/*
 * First candidate of final rcx_hash_list_add()
 *
 * Returns zero only
 */
int rcx_hash_list_fb1_add(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	rcx_list_fb1_add(g_hash_list->buckets[hash], val);
	return 0;
}

/*
 * Deletes a value from the global hash list
 *
 * It should have a fallback mechanism for transaction abort
 *
 * Returns -1 if aborted, zero else
 */
int rcx_hash_list_remove(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);
	int result = rcx_list_remove(g_hash_list->buckets[hash], val);

	if (result == 2)
		return -1;
	else
		return 0;
}

/*
 * try-and-fail version of rcx_hash_list_remove()
 *
 * Returns -1 if aborted, zero else
 */
int rcx_hash_list_try_remove(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);
	int result = rcx_list_remove(g_hash_list->buckets[hash], val);

	if (result == 2)
		return -1;
	else
		return 0;
}

/*
 * Retry version of rcx_hash_list_try_remove()
 *
 * If it aborts, it retries until success but return iirescoverable abort if it
 * cannot success until the benchmark time finished.
 *
 * Returns 0 for success, -1 for irrecoverable aborts
 */
int rcx_hash_list_retry_remove(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);
	int result;

	for (; ;) {
		result = rcx_list_remove(g_hash_list->buckets[hash], val);
		if (result != 2)
			break;
		if (benchmark_endtime() != 1)
			continue;
		return -1;
	}

	return 0;
}

/*
 * Locking fallback version of rcx_hash_list_remove()
 *
 * Returns zero only
 */
int rcx_hash_list_lf_remove(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	rcx_list_lf_remove(g_hash_list->buckets[hash], val);
	return 0;
}

/*
 * First candidate of final rcx_hash_list_remove()
 *
 * Returns zero only
 */
int rcx_hash_list_fb1_remove(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	rcx_list_fb1_remove(g_hash_list->buckets[hash], val);
	return 0;
}

int rcx_hash_list_htmlock_add(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	rcx_list_htmlock_add(g_hash_list->buckets[hash], val);
	return 0;
}

int rcx_hash_list_hhtmlock_add(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	rcx_list_hhtmlock_add(g_hash_list->buckets[hash], val);
	return 0;
}

int rcx_hash_list_numa_add(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	rcx_list_numa_add(g_hash_list->buckets[hash], val);
	return 0;
}

int rcx_hash_list_htmlock_remove(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	rcx_list_htmlock_remove(g_hash_list->buckets[hash], val);
	return 0;
}

int rcx_hash_list_hhtmlock_remove(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	rcx_list_hhtmlock_remove(g_hash_list->buckets[hash], val);
	return 0;
}

int rcx_hash_list_numa_remove(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	rcx_list_numa_remove(g_hash_list->buckets[hash], val);
	return 0;
}
