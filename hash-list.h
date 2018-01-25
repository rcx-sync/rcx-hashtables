#ifndef _HASH_LIST_H_
#define _HASH_LIST_H_

/////////////////////////////////////////////////////////
// INCLUDES
/////////////////////////////////////////////////////////
#include <linux/types.h>

/////////////////////////////////////////////////////////
// DEFINES
/////////////////////////////////////////////////////////
#define LIST_VAL_MIN (INT_MIN)
#define LIST_VAL_MAX (INT_MAX)

#define NODE_PADDING (30)
#define CACHELINE_SIZE (128)

#define MAX_BUCKETS (1000)
#define DEFAULT_BUCKETS                 1

#define NR_NUMA_NODES (4)

/////////////////////////////////////////////////////////
// TYPES
/////////////////////////////////////////////////////////
typedef int val_t;

typedef union aligned_spinlock {
	spinlock_t __attribute__((aligned(CACHELINE_SIZE))) lock;
	char padding[CACHELINE_SIZE];
} aligned_spinlock_t;

typedef union node node_t;
typedef union node {
	struct {
		val_t val;
		node_t *p_next;
		int removed;
		struct rcu_head rcu;
		/* per-NUMA node locks */
		union {
			char __attribute__((aligned(CACHELINE_SIZE)))
				pnode_locks[CACHELINE_SIZE * NR_NUMA_NODES];

			aligned_spinlock_t
				__attribute__((aligned(CACHELINE_SIZE)))
				pnd_slocks[NR_NUMA_NODES];
		};

		/* global lock */
		union {
			spinlock_t __attribute__((aligned(CACHELINE_SIZE)))
				global_lock;
			char __attribute__((aligned(CACHELINE_SIZE)))
				global_htmlock;
		};
	};
	char * padding[CACHELINE_SIZE];
} node_t;

typedef union list {
	struct {
		node_t *p_head;
		spinlock_t rcuspin;
	};
	char *padding[CACHELINE_SIZE];
} list_t;

typedef struct hash_list {
	int n_buckets;
	list_t *buckets[MAX_BUCKETS];
	char *padding[CACHELINE_SIZE];
} hash_list_t;

/////////////////////////////////////////////////////////
// INTERFACE
/////////////////////////////////////////////////////////
hash_list_t *rcu_new_hash_list(int n_buckets);
hash_list_t *rlu_new_hash_list(int n_buckets);
hash_list_t *rcx_new_hash_list(int n_buckets);

int rcu_hash_list_init(int nr_buckets, void *dat);
int rcu_hash_list_contains(void *tl, val_t val);
int rcu_hash_list_add(void *tl, val_t val);
int rcu_hash_list_remove(void *tl, val_t val);
int rcu_hash_list_try_add(void *tl, val_t val);
int rcu_hash_list_try_remove(void *tl, val_t val);
int rcu_hash_list_fg_add(void *tl, val_t val);
int rcu_hash_list_fg_remove(void *tl, val_t val);
int rcu_hash_list_numa_add(void *tl, val_t val);
int rcu_hash_list_numa_remove(void *tl, val_t val);
void rcu_hash_list_destroy(void);

int rlu_hash_list_init(int nr_buckets, void *dat);
int rlu_hash_list_contains(void *self, val_t val);
int rlu_hash_list_add(void *self, val_t val);
int rlu_hash_list_remove(void *self, val_t val);
int rlu_hash_list_try_add(void *self, val_t val);
int rlu_hash_list_try_remove(void *self, val_t val);
void rlu_hash_list_destroy(void);

int rcx_hash_list_init(int nr_buckets, void *dat);
int rcx_hash_list_contains(void *tl, val_t val);
int rcx_hash_list_add(void *tl, val_t val);
int rcx_hash_list_remove(void *tl, val_t val);
int rcx_hash_list_try_add(void *tl, val_t val);
int rcx_hash_list_try_remove(void *tl, val_t val);
int rcx_hash_list_retry_add(void *tl, val_t val);
int rcx_hash_list_retry_remove(void *tl, val_t val);
int rcx_hash_list_lf_add(void *tl, val_t val);
int rcx_hash_list_lf_remove(void *tl, val_t val);
int rcx_hash_list_fb1_add(void *tl, val_t val);
int rcx_hash_list_fb1_remove(void *tl, val_t val);
int rcx_hash_list_htmlock_add(void *tl, val_t val);
int rcx_hash_list_htmlock_remove(void *tl, val_t val);
int rcx_hash_list_hhtmlock_add(void *tl, val_t val);
int rcx_hash_list_hhtmlock_remove(void *tl, val_t val);
int rcx_hash_list_numa_add(void *tl, val_t val);
int rcx_hash_list_numa_remove(void *tl, val_t val);
void rcx_hash_list_destroy(void);

#endif // _HASH_LIST_H_
