#include <linux/slab.h>  // kmalloc
#include <linux/rcupdate.h>
#include <linux/types.h>

#include "hash-list.h"

#define HASH_VALUE(p_hash_list, val)    (val % p_hash_list->n_buckets)

#define RCU_READER_LOCK()               rcu_read_lock()
#define RCU_READER_UNLOCK()             rcu_read_unlock()
#define RCU_SYNCHRONIZE()               synchronize_rcu()
#define RCU_ASSIGN_PTR(p_ptr, p_obj)    rcu_assign_pointer(p_ptr, p_obj)

#define RCU_DEREF(p_obj)                (p_obj)

#define RCU_WRITER_LOCK(lock)           spin_lock(&lock)
#define RCU_WRITER_UNLOCK(lock)         spin_unlock(&lock)
#define RCU_FREE(ptr)                   kfree_rcu(ptr, rcu)

__cacheline_aligned static hash_list_t *g_hash_list;

#define pndslock(node) \
	(node->pnd_slocks[numa_node_id()].lock)

#define pndslockof(node, nodeid) \
	(node->pnd_slocks[nodeid].lock)

/* Allocate a node */
node_t *rcu_new_node(void)
{
	int nodeid;
	node_t *p_new_node = kmalloc(sizeof(node_t), GFP_KERNEL);

	if (p_new_node == NULL)
		return NULL;

	p_new_node->removed = 0;
	for_each_node_with_cpus(nodeid)
		pndslockof(p_new_node, nodeid) = __SPIN_LOCK_UNLOCKED(
				pndslockof(p_new_node, nodeid));

	p_new_node->global_lock = __SPIN_LOCK_UNLOCKED(p_new_node->global_lock);

	return p_new_node;
}

/* Free a node */
void rcu_free_node(node_t *p_node)
{
	RCU_FREE(p_node);
}

/* Allocate and initialize a list */
list_t *rcu_new_list(void)
{
	list_t *p_list;
	node_t *p_min_node, *p_max_node;

	p_list = kmalloc(sizeof(list_t), GFP_KERNEL);
	if (p_list == NULL)
		return NULL;

	p_max_node = rcu_new_node();
	p_max_node->val = LIST_VAL_MAX;
	p_max_node->p_next = NULL;

	p_min_node = rcu_new_node();
	p_min_node->val = LIST_VAL_MIN;
	p_min_node->p_next = p_max_node;

	p_list->p_head = p_min_node;
	p_list->rcuspin = __SPIN_LOCK_UNLOCKED(p_list->rcuspin);

	return p_list;
}

/* Allocate and initialize a hash list */
hash_list_t *rcu_new_hash_list(int n_buckets)
{
	int i;
	hash_list_t *p_hash_list;

	p_hash_list = kmalloc(sizeof(hash_list_t), GFP_KERNEL);

	if (p_hash_list == NULL)
		return NULL;

	p_hash_list->n_buckets = n_buckets;

	for (i = 0; i < p_hash_list->n_buckets; i++)
		p_hash_list->buckets[i] = rcu_new_list();

	return p_hash_list;
}

/* Initialize the global hash list */
int rcu_hash_list_init(int nr_buckets, void *dat)
{
	g_hash_list = rcu_new_hash_list(nr_buckets);
	return 0;
}

/* Returns number of entries in the given list */
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

/* Returns number of entries in the given hash list */
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
 * Check whether a list is containing a value
 *
 * Returns one if containing, zero else
 */
int rcu_list_contains(list_t *p_list, val_t val)
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
 * Check whether a hash list is containing a value
 *
 * Returns zero if containing, -ENOENT else
 */
int rcu_hash_list_contains(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	return rcu_list_contains(g_hash_list->buckets[hash], val) ?
		0 : -ENOENT;
}

/*
 * Add a value into a list
 */
int rcu_list_add(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	val_t v;

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
		node_t *p_new_node = rcu_new_node();

		p_new_node->val = val;
		p_new_node->p_next = p_next;

		RCU_ASSIGN_PTR((p_prev->p_next), p_new_node);
	}

	RCU_WRITER_UNLOCK(p_list->rcuspin);

	return 0;
}

/*
 * Try and fail version of rcu_list_add()
 *
 * Returns two immediately as soon as conflict is detected, zero if success.
 */
int rcu_list_try_add(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	val_t v;

	if (!spin_trylock(&p_list->rcuspin))
		return 2;

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
		node_t *p_new_node = rcu_new_node();

		p_new_node->val = val;
		p_new_node->p_next = p_next;

		RCU_ASSIGN_PTR((p_prev->p_next), p_new_node);
	}

	RCU_WRITER_UNLOCK(p_list->rcuspin);

	return 0;
}

/*
 * Finer-grained locking version of rcu_list_try_add()
 *
 * Returns two if conflict detected, zero if success
 */
int rcu_list_fg_add(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	val_t v;

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
		node_t *p_new_node = rcu_new_node();

		p_new_node->val = val;
		p_new_node->p_next = p_next;

		spin_lock(&p_prev->global_lock);
		spin_lock(&p_next->global_lock);

		if (RCU_DEREF(p_prev->p_next) != p_next)
			goto unlock_retry;

		if (p_prev->removed || p_next->removed)
			goto unlock_retry;

		RCU_ASSIGN_PTR((p_prev->p_next), p_new_node);

		spin_unlock(&p_next->global_lock);
		spin_unlock(&p_prev->global_lock);

		return result;

unlock_retry:
		spin_unlock(&p_next->global_lock);
		spin_unlock(&p_prev->global_lock);

		kfree(p_new_node);
		goto retry;
	}

	return result;
}

/*
 * NUMA-aware fine-grained locking version of rcu_list_try_add()
 *
 * Returns one if the value is in the list already, or zero if success.
 */
int rcu_list_numa_add(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	val_t v;

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
		node_t *p_new_node = rcu_new_node();

		p_new_node->val = val;
		p_new_node->p_next = p_next;

		spin_lock(&pndslock(p_prev));
		spin_lock(&pndslock(p_next));

		spin_lock(&p_prev->global_lock);
		spin_lock(&p_next->global_lock);

		if (RCU_DEREF(p_prev->p_next) != p_next)
			goto unlock_retry;

		if (p_prev->removed || p_next->removed)
			goto unlock_retry;

		RCU_ASSIGN_PTR((p_prev->p_next), p_new_node);

		spin_unlock(&p_next->global_lock);
		spin_unlock(&p_prev->global_lock);

		spin_unlock(&pndslock(p_next));
		spin_unlock(&pndslock(p_prev));

		return result;

unlock_retry:
		spin_unlock(&p_next->global_lock);
		spin_unlock(&p_prev->global_lock);

		spin_unlock(&pndslock(p_next));
		spin_unlock(&pndslock(p_prev));
		kfree(p_new_node);
		goto retry;
	}

	return result;
}

/*
 * Insert a value into the global hash list
 *
 * Returns zero always
 */
int rcu_hash_list_add(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	return rcu_list_add(g_hash_list->buckets[hash], val);
}

/*
 * Try and abort version of rcu_hash_list_add()
 *
 * Returns zero for success, two for conflicts
 */
int rcu_hash_list_try_add(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	return rcu_list_try_add(g_hash_list->buckets[hash], val);
}

/*
 * Finer-grained locking version of rcu_hash_list_try_add()
 *
 * Returns zero for success, two for conflicts
 */
int rcu_hash_list_fg_add(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	return rcu_list_fg_add(g_hash_list->buckets[hash], val);
}

/*
 * Numa aware locking version of rcu_hash_list_try_add()
 *
 * Returns zero for success, two for conflicts
 */
int rcu_hash_list_numa_add(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	return rcu_list_numa_add(g_hash_list->buckets[hash], val);
}

/*
 * Delete a value from a list
 *
 * Always return zero
 */
int rcu_list_remove(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	node_t *n;

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

		RCU_WRITER_UNLOCK(p_list->rcuspin);

		rcu_free_node(p_next);

		return 0;
	}

	RCU_WRITER_UNLOCK(p_list->rcuspin);

	return 0;
}

/*
 * Try-and-fail version of rcu_list_remove()
 *
 * Returns two if conflict detected, zero if success
 */
int rcu_list_try_remove(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	node_t *n;

	if (!spin_trylock(&p_list->rcuspin))
		return 2;

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

		RCU_WRITER_UNLOCK(p_list->rcuspin);

		rcu_free_node(p_next);

		return 0;
	}

	RCU_WRITER_UNLOCK(p_list->rcuspin);

	return 0;
}

/*
 * Finer-grained locking version of rcu_list_try_remove()
 *
 * Returns two if conflict detected, zero if success
 */
int rcu_list_fg_remove(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	node_t *n;

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

		spin_lock(&p_prev->global_lock);
		spin_lock(&p_next->global_lock);
		spin_lock(&n->global_lock);

		if (p_prev->removed || p_next->removed || n->removed)
			goto unlock_retry;

		if (RCU_DEREF(p_prev->p_next) != p_next ||
				RCU_DEREF(p_next->p_next) != n)
			goto unlock_retry;

		RCU_ASSIGN_PTR((p_prev->p_next), n);
		p_next->removed = 1;
		rcu_free_node(p_next);

		spin_unlock(&n->global_lock);
		spin_unlock(&p_next->global_lock);
		spin_unlock(&p_prev->global_lock);

		return result;

unlock_retry:
		spin_unlock(&n->global_lock);
		spin_unlock(&p_next->global_lock);
		spin_unlock(&p_prev->global_lock);

		goto retry;
	}

	return result;
}

/*
 * NUMA-aware locking version of rcu_list_try_remove()
 *
 * Returns 1 if success, 0 if the list doesn't contain the value.
 */
int rcu_list_numa_remove(list_t *p_list, val_t val)
{
	int result;
	node_t *p_prev, *p_next;
	node_t *p_node;
	node_t *n;

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
		spin_lock(&pndslock(p_prev));
		spin_lock(&pndslock(p_next));
		spin_lock(&pndslock(n));

		spin_lock(&p_prev->global_lock);
		spin_lock(&p_next->global_lock);
		spin_lock(&n->global_lock);

		if (p_prev->removed || p_next->removed || n->removed)
			goto unlock_retry;

		if (RCU_DEREF(p_prev->p_next) != p_next ||
				RCU_DEREF(p_next->p_next) != n)
			goto unlock_retry;

		RCU_ASSIGN_PTR((p_prev->p_next), n);
		p_next->removed = 1;
		rcu_free_node(p_next);

		spin_unlock(&n->global_lock);
		spin_unlock(&p_next->global_lock);
		spin_unlock(&p_prev->global_lock);

		spin_unlock(&pndslock(n));
		spin_unlock(&pndslock(p_next));
		spin_unlock(&pndslock(p_prev));

		return result;

unlock_retry:
		spin_unlock(&n->global_lock);
		spin_unlock(&p_next->global_lock);
		spin_unlock(&p_prev->global_lock);

		spin_unlock(&pndslock(n));
		spin_unlock(&pndslock(p_next));
		spin_unlock(&pndslock(p_prev));
		goto retry;
	}

	return result;
}

/*
 * Remove a value from the global hash list
 *
 * Returns zero always
 */
int rcu_hash_list_remove(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	return rcu_list_remove(g_hash_list->buckets[hash], val);
}

/*
 * Try-and-fail version of rcu_hash_list_remove()
 *
 * Returns two if conflict detected, zero if success
 */
int rcu_hash_list_try_remove(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	return rcu_list_try_remove(g_hash_list->buckets[hash], val);
}

/*
 * Finer-grained locking version of rcu_hash_list_try_remove()
 *
 * Returns two if conflict detected, zero if success
 */
int rcu_hash_list_fg_remove(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	return rcu_list_fg_remove(g_hash_list->buckets[hash], val);
}

/*
 * NUMA-aware locking version of rcu_hash_list_try_remove()
 *
 * Returns two if conflict detected, zero if success
 */
int rcu_hash_list_numa_remove(void *tl, val_t val)
{
	int hash = HASH_VALUE(g_hash_list, val);

	return rcu_list_numa_remove(g_hash_list->buckets[hash], val);
}

static void rcu_list_destroy(list_t *list)
{
	node_t *iter;

	for (iter = (node_t *)RCU_DEREF(list->p_head);
			iter != NULL;
			iter = iter->p_next) {
		list->p_head = iter->p_next;
		kfree(iter);
	}
}

void rcu_hash_list_destroy(void)
{
	int hash;

	for (hash = 0; hash < g_hash_list->n_buckets; hash++) {
		rcu_list_destroy(g_hash_list->buckets[hash]);
		kfree(g_hash_list->buckets[hash]);
	}
	kfree(g_hash_list);
}
