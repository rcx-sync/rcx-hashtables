#include <linux/module.h>
#include <linux/moduleparam.h>  /* module_param */
#include <linux/kernel.h>       /* pr_info/printk */
#include <linux/errno.h>        /* errors code */
#include <linux/sched.h>        /* current global variable */
#include <linux/init.h>         /* __init __exit macros */
#include <linux/kthread.h>      /* kthreads */
#include <linux/completion.h>   /* complete/wait_for_completion */
#include <linux/delay.h>        /* msleep */
#include <linux/slab.h>         /* kmalloc/kzalloc */
#include <linux/mm.h>		/* kvmalloc */
#include <linux/random.h>       /* rnd_state/ */
#include <asm/timex.h>          /* get_cycles (same as rdtscll) */

#include "sync_test.h"
#include "barrier.h"
#include "rlu.h"
#include "hash-list.h"

#include "rtm_debug.h"

#define MODULE_NAME    "sync_test"
#define MAX_BENCHMARKS (16)
#ifndef RLU_DEFER_WS
# define RLU_DEFER_WS  (10)
#endif
#define FORCE_SCHED    (1) /* Enable this define to allow long execution */

/* Bench configuration */
static char *benchmark = "rcuhashlist";
module_param(benchmark, charp, 0000);
MODULE_PARM_DESC(benchmark, "Benchmark name");
static int threads_nb = 1;
module_param(threads_nb, int, 0000);
MODULE_PARM_DESC(threads_nb, "Number of threads");
static int duration = 100;
module_param(duration, int, 0000);
MODULE_PARM_DESC(duration, "Duration of the benchmark in ms");
static int update;
module_param(update, int, 0000);
MODULE_PARM_DESC(update, "Probability for update operations. No floating-point in kernel so 10000 = 100%, 1 = 0.01%");
static int range = 1024;
module_param(range, int, 0000);
MODULE_PARM_DESC(range, "Key range. Initial set size is half the key range.");
static int nr_buckets = 1;
module_param(nr_buckets, int, 0000);
MODULE_PARM_DESC(nr_buckets, "Number of buckets to utilize.  Defaults to 1.");

typedef struct benchmark {
	char name[32];
	int (*init)(int nr_buckets, void *dat);
	int (*lookup)(void *tl, int key);
	int (*insert)(void *tl, int key);
	int (*delete)(void *tl, int key);
	void (*destroy)(void);
	unsigned long nb_lookup;
	unsigned long nb_insert;
	unsigned long nb_delete;
	unsigned long nb_ins_abort;
	unsigned long nb_del_abort;
} benchmark_t;

static benchmark_t benchmarks[MAX_BENCHMARKS] = {
	{
		.name = "rcu",
		.init = &rcu_hash_list_init,
		.lookup = &rcu_hash_list_contains,
		.insert = &rcu_hash_list_add,
		.delete = &rcu_hash_list_remove,
		.destroy = &rcu_hash_list_destroy,
	},
	{
		.name = "rcu-forgive",	/* try and forgive */
		.init = &rcu_hash_list_init,
		.lookup = &rcu_hash_list_contains,
		.insert = &rcu_hash_list_try_add,
		.delete = &rcu_hash_list_try_remove,
		.destroy = &rcu_hash_list_destroy,
	},
	{
		.name = "rcu-fglock",	/* finer-grained locking */
		.init = &rcu_hash_list_init,
		.lookup = &rcu_hash_list_contains,
		.insert = &rcu_hash_list_fg_add,
		.delete = &rcu_hash_list_fg_remove,
		.destroy = &rcu_hash_list_destroy,
	},
	{
		.name = "rcu-numa",	/* finer-grained locking */
		.init = &rcu_hash_list_init,
		.lookup = &rcu_hash_list_contains,
		.insert = &rcu_hash_list_numa_add,
		.delete = &rcu_hash_list_numa_remove,
		.destroy = &rcu_hash_list_destroy,
	},
	{
		.name = "rlu",
		.init = &rlu_hash_list_init,
		.lookup = &rlu_hash_list_contains,
		.insert = &rlu_hash_list_add,
		.delete = &rlu_hash_list_remove,
		.destroy = NULL,
	},
	{
		.name = "rlu-forgive",
		.init = &rlu_hash_list_init,
		.lookup = &rlu_hash_list_contains,
		.insert = &rlu_hash_list_try_add,
		.delete = &rlu_hash_list_try_remove,
		.destroy = NULL,
	},
	{
		.name = "rcuhtm",
		.init = &rcx_hash_list_init,
		.lookup = &rcx_hash_list_contains,
		.insert = &rcx_hash_list_lf_add,
		.delete = &rcx_hash_list_lf_remove,
		.destroy = &rcx_hash_list_destroy,
	},
	{
		.name = "forgive", /* forgive if trx aborts */
		.init = &rcx_hash_list_init,
		.lookup = &rcx_hash_list_contains,
		.insert = &rcx_hash_list_try_add,
		.delete = &rcx_hash_list_try_remove,
		.destroy = &rcx_hash_list_destroy,
	},
	{
		.name = "retry",	/* retry the trx until success */
		.init = &rcx_hash_list_init,
		.lookup = &rcx_hash_list_contains,
		.insert = &rcx_hash_list_retry_add,
		.delete = &rcx_hash_list_retry_remove,
		.destroy = &rcx_hash_list_destroy,
	},
	{
		.name = "hwa",		/* retry or fallback as hw advised */
		.init = &rcx_hash_list_init,
		.lookup = &rcx_hash_list_contains,
		.insert = &rcx_hash_list_fb1_add,
		.delete = &rcx_hash_list_fb1_remove,
		.destroy = &rcx_hash_list_destroy,
	},
	{
		.name = "rcx-htmlock",	/* hierarchical htm global lock */
		.init = &rcx_hash_list_init,
		.lookup = &rcx_hash_list_contains,
		.insert = &rcx_hash_list_htmlock_add,
		.delete = &rcx_hash_list_htmlock_remove,
		.destroy = &rcx_hash_list_destroy,
	},
	{
		.name = "rcx-hhtmlock",	/* hierarchical htm global lock */
		.init = &rcx_hash_list_init,
		.lookup = &rcx_hash_list_contains,
		.insert = &rcx_hash_list_hhtmlock_add,
		.delete = &rcx_hash_list_hhtmlock_remove,
		.destroy = &rcx_hash_list_destroy,
	},
	{
		.name = "rcx",
		.init = &rcx_hash_list_init,
		.lookup = &rcx_hash_list_contains,
		.insert = &rcx_hash_list_numa_add,
		.delete = &rcx_hash_list_numa_remove,
		.destroy = &rcx_hash_list_destroy,
	},
};

typedef struct benchmark_thread {
	benchmark_t *benchmark;
	unsigned int id;
	rlu_thread_data_t *rlu;
	struct rnd_state rnd;
	struct {
		unsigned long nb_lookup;
		unsigned long nb_insert;
		unsigned long nb_delete;
		unsigned long nb_ins_abort;
		unsigned long nb_del_abort;
	} ops;
} benchmark_thread_t;

benchmark_thread_t *benchmark_threads[RLU_MAX_THREADS];

struct completion sync_test_working;

__cacheline_aligned static barrier_t sync_test_barrier;

/* Generate a pseudo random in range [0:n[ */
static inline int rand_range(int n, struct rnd_state *seed)
{
	return prandom_u32_state(seed) % n;
}

__cacheline_aligned static struct timespec bench_start_time;

/*
 * Return 1 if it is time to end benchmark
 */
int benchmark_endtime(void)
{
	struct timespec end;
	unsigned long duration_ms;

	end = current_kernel_time();
	duration_ms = (end.tv_sec * 1000 + end.tv_nsec / 1000 / 1000) -
		(bench_start_time.tv_sec * 1000 +
		 bench_start_time.tv_nsec / 1000 / 1000);
	if (duration_ms >= duration)
		return 1;
	return 0;
}

static int sync_test_thread(void *data)
{
	benchmark_thread_t *bench = (benchmark_thread_t *)data;
	unsigned long duration_ms;
	struct timespec start, end;
	unsigned long long tsc_start, tsc_end;
	rlu_thread_data_t *self = bench->rlu;

	/* Wait on barrier */
	barrier_cross(&sync_test_barrier);

	tsc_start = get_cycles();
	start = current_kernel_time();

	/* Thread main */
	do {
		int op = rand_range(10000, &bench->rnd);
		int val = rand_range(range, &bench->rnd);

		if (op < update) {
			op = rand_range(2, &bench->rnd);
			if ((op & 1) == 0) {
				/* Insert */
				if (bench->benchmark->insert(self, val) == 0) {
					bench->ops.nb_insert++;
				} else {
					bench->ops.nb_ins_abort++;
				}
			} else {
				/* Delete */
				if (bench->benchmark->delete(self, val) == 0) {
					bench->ops.nb_delete++;
				} else {
					bench->ops.nb_del_abort++;
				}
			}
		} else {
			/* Lookup */
			bench->benchmark->lookup(self, val);
			bench->ops.nb_lookup++;
		}
		end = current_kernel_time();
		duration_ms = (end.tv_sec * 1000 + end.tv_nsec / 1000000) -
			(start.tv_sec * 1000 + start.tv_nsec / 1000000);
#ifdef FORCE_SCHED
		/* No need to force schedule(), time bound. */
		cond_resched();
#endif /* FORCE_SCHED */
	} while (duration_ms < duration);

	tsc_end = get_cycles();
	pr_info(MODULE_NAME "(%i:%i) time: %lu ms (%llu cycles)\n",
			current->pid, bench->id, duration_ms,
			tsc_end - tsc_start);

	/* Thread finishing */
	complete(&sync_test_working);
	rlu_thread_finish(bench->rlu);

	return 0;
}

/*
 * NOTE: Main thread doesn't be bounded to a cpu.  For that, use taskset when
 * you insmod the module.
 */
#define BIND_CPU_NO	0	/* Do not bind cpu */
#define BIND_CPU_SEQ	1	/* Just sequentially bind cpu */
#define BIND_CPU_NUMA	2	/* Use minimal number of NUMA nodes */
#define BIND_CPU	BIND_CPU_NUMA

#if BIND_CPU == BIND_CPU_SEQ
static int *cpubind_seq_arr(int nr_threads)
{
	int *array;
	int thr;

	array = kmalloc(sizeof(int) * nr_threads, GFP_KERNEL);
	for (thr = 0; thr < nr_threads; thr++)
		array[thr] = thr;

	return array;
}
#endif

#if BIND_CPU == BIND_CPU_NUMA
static int *cpubind_numa_arr(int nr_threads)
{
	int *array;
	int node, cpu, thr, capa;
	const struct cpumask *cpus;

	array = kmalloc(sizeof(int) * nr_threads, GFP_KERNEL);
	node = first_online_node;
	capa = nr_cpus_node(node);
	cpus = cpumask_of_node(node);
	cpu = cpumask_first(cpus);
	for (thr = 0; thr < nr_threads; thr++) {
		if (capa <= thr) {
			node = next_online_node(node);
			if (node == MAX_NUMNODES)
				node = first_online_node;
			capa += nr_cpus_node(node);
		}
		if (cpu >= nr_cpu_ids) {
			cpus = cpumask_of_node(node);
			cpu = cpumask_first(cpus);
		}
		array[thr] = cpu;
		cpu = cpumask_next(cpu, cpus);
	}

	return array;
}
#endif

static int __init sync_test_init(void)
{
	benchmark_t *bench = NULL;
	int i;
	long nr_ops, nr_updates, nr_aborts;
	struct result_stat restat;
#if BIND_CPU
	int *thread_cpu_map;
#endif

	/* Benchmark to run */
	for (i = 0; i < MAX_BENCHMARKS; i++) {
		if (benchmarks[i].name &&
				!strcmp(benchmarks[i].name, benchmark)) {
			bench = &benchmarks[i];
			break;
		}
	}

	/* Error check */
	if (!bench) {
		pr_err(MODULE_NAME ": Unknown benchmark %s\n", benchmark);
		return -EPERM;
	}
	if (bench->lookup == NULL || bench->insert == NULL ||
			bench->delete == NULL) {
		pr_err(MODULE_NAME ": Benchmark %s has a NULL function defined\n",
				benchmark);
		return -EPERM;
	}
	pr_notice(MODULE_NAME ": Running benchmark %s with %i threads\n",
			benchmark, threads_nb);

	if (threads_nb > num_online_cpus()) {
		pr_err(MODULE_NAME ": Invalid number of threads %d (MAX %d)\n",
				threads_nb, num_online_cpus());
		return -EPERM;
	}
	if (threads_nb > RLU_MAX_THREADS) {
		pr_err(MODULE_NAME ": Invalid number of threads %d (MAX %d)\n",
				threads_nb, RLU_MAX_THREADS);
		return -EPERM;
	}
	if (nr_buckets > MAX_BUCKETS) {
		pr_err(MODULE_NAME ": Invalid number of buckets %d (MAX %d)\n",
				nr_buckets, MAX_BUCKETS);
		return -EPERM;
	}
	/* RLU stalls when 144 threads used */
	if (!strcmp(bench->name, "rlu") && threads_nb >= 144)
		goto print_result;

	/* Initialization */
	init_completion(&sync_test_working);
	barrier_init(&sync_test_barrier, threads_nb);
	rlu_init(RLU_TYPE_FINE_GRAINED, RLU_DEFER_WS);
	bench->init(nr_buckets, NULL);
	for (i = 0; i < threads_nb; i++) {
		benchmark_threads[i] = kzalloc(sizeof(*benchmark_threads[i]),
				GFP_KERNEL);
		if (benchmark_threads[i] == NULL)
			return -ENOMEM;

		benchmark_threads[i]->id = i;
		prandom_seed_state(&benchmark_threads[i]->rnd, i + 1);
		benchmark_threads[i]->benchmark = bench;
		/* Other fields are set to 0 with kzalloc */
		/* RLU Thread initialization */
		benchmark_threads[i]->rlu = kvmalloc(sizeof(rlu_thread_data_t),
				GFP_KERNEL);
		if (benchmark_threads[i]->rlu == NULL)
			return -ENOMEM;

		rlu_thread_init(benchmark_threads[i]->rlu);
	}

	/* Half fill the set */
	for (i = 0; i < range / 2; i++) {
		/* rcu-fglock and rcu-numa should use rcu_hash_list_add() */
		if (!strncmp(bench->name, "rcu-", 4)) {
			while (rcu_hash_list_add(NULL,
						get_random_int() % range))
				;
			continue;
		}
		/* Ensure the success of insertion */
		while (bench->insert(benchmark_threads[0]->rlu,
					get_random_int() % range))
			;
	}

	/* Start N-1 threads */
#if BIND_CPU == BIND_CPU_SEQ
	thread_cpu_map = cpubind_seq_arr(threads_nb);
#elif BIND_CPU == BIND_CPU_NUMA
	thread_cpu_map = cpubind_numa_arr(threads_nb);
#endif

	bench_start_time = current_kernel_time();
	for (i = 1; i < threads_nb; i++) {
		struct task_struct *t;
		/* kthread_run() can be also used to avoid wake_up_process */
		t = kthread_create(sync_test_thread, benchmark_threads[i],
				"sync_test_thread");
		if (t) {
			pr_notice(MODULE_NAME ": pid: %d (created from %d)\n",
					t->pid, current->pid);
#if BIND_CPU
			kthread_bind(t, thread_cpu_map[i]);
#endif
			wake_up_process(t);
		}
	}

	/* Main thread is also doing work. */
	sync_test_thread(benchmark_threads[0]);

	/* Wait for the threads to finish */
	for (i = 0; i < threads_nb; i++) {
		pr_debug(MODULE_NAME ": Waiting still %d threads to finish\n",
				i);
		wait_for_completion(&sync_test_working);
	}

#if BIND_CPU
	kfree(thread_cpu_map);
#endif

	/* Reinitialize one thread to cleanup */
	rlu_thread_init(benchmark_threads[0]->rlu);

	/* Statistics output */
	for (i = 0; i < threads_nb; i++) {
		bench->nb_lookup += benchmark_threads[i]->ops.nb_lookup;
		bench->nb_insert += benchmark_threads[i]->ops.nb_insert;
		bench->nb_delete += benchmark_threads[i]->ops.nb_delete;
		bench->nb_ins_abort += benchmark_threads[i]->ops.nb_ins_abort;
		bench->nb_del_abort += benchmark_threads[i]->ops.nb_del_abort;
	}

print_result:
	pr_info(MODULE_NAME ": #lookup: %lu / s\n", bench->nb_lookup * 1000 /
				duration);
	pr_info(MODULE_NAME ": #insert: %lu / s\n", bench->nb_insert * 1000 /
				duration);
	pr_info(MODULE_NAME ": #delete: %lu / s\n", bench->nb_delete * 1000 /
				duration);
	pr_info(MODULE_NAME ": #update: %lu / s\n", (bench->nb_delete +
				bench->nb_insert) * 1000 / duration);
	nr_aborts = bench->nb_ins_abort + bench->nb_del_abort;
	nr_ops = bench->nb_lookup + bench->nb_insert + bench->nb_delete +
		nr_aborts;
	if (nr_ops == 0)
		nr_ops = 1;
	pr_info(MODULE_NAME ": #ops: %lu / s\n", (nr_ops * 1000 / duration));
	pr_info(MODULE_NAME ": #success: %lu / s\n", (nr_ops - nr_aborts) *
				1000 / duration);
	pr_info(MODULE_NAME ": #ins abort: %lu / s\n", bench->nb_ins_abort *
				1000 / duration);
	pr_info(MODULE_NAME ": #del abort: %lu / s\n", bench->nb_del_abort *
				1000 / duration);
	pr_info(MODULE_NAME ": #abort: %lu / s\n", nr_aborts * 1000 /
				duration);
	pr_info(MODULE_NAME ": #abort / ops : %lu / 1000 ops\n",
				nr_aborts * 1000 / nr_ops);

	nr_updates = bench->nb_insert + bench->nb_delete + nr_aborts;
	if (nr_updates == 0)
		nr_updates = 1;
	pr_info(MODULE_NAME ": #abort / updates : %lu / 1000 updates\n",
				nr_aborts * 1000 / nr_updates);

	restat.duration_ms = duration;
	restat.nr_issued_ops = nr_ops;
	restat.nr_succ_ops = nr_ops - nr_aborts;
	restat.nr_upd = nr_updates;
	pr_abort_stat(&restat);

	/* RLU stalls when 144 threads used */
	if (!strcmp(bench->name, "rlu") && threads_nb >= 144)
		goto end;


	if (bench->destroy) {
		pr_info(MODULE_NAME ": destroy!\n");
		bench->destroy();
		goto cleaning;
	}
	/*
	 * In case of RLU, we should delete each item before destroying the
	 * meta-data for hash table.
	 */
	for (i = 0; i < range; i++) {
		bench->delete(benchmark_threads[0]->rlu, i);
		cond_resched();
	}
	rlu_thread_finish(benchmark_threads[0]->rlu);
	rlu_hash_list_destroy();

cleaning:
	for (i = 0; i < threads_nb; i++)
		kvfree(benchmark_threads[i]->rlu);

	rlu_finish();

end:
	/*
	 * When the benchmark is done, the module is loaded. Maybe we can fail
	 * anyway to avoid empty unload.
	 */
	pr_info(MODULE_NAME ": Done\n");
	return 0;
}

static void __exit sync_test_exit(void)
{
	pr_info(MODULE_NAME ": Unloaded\n");
}

module_init(sync_test_init);
module_exit(sync_test_exit);

MODULE_LICENSE("GPL");
