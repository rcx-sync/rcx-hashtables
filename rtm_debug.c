#include <linux/percpu.h>

#include "rtm_debug.h"
#include "rtm.h"

struct rtm_abort_cnt {
	unsigned long counts[NR_ABORT_REASONS];
	unsigned long nr_aborts;
};

DEFINE_PER_CPU(struct rtm_abort_cnt, abort_cnt);
static char *str_abort_reasons[NR_ABORT_REASONS] = {
	"rtm_explicit", "rtm_retry", "rtm_conflict", "rtm_capa", "rtm_dbg",
	"rtm_nest", "double free", "conflict", "lfconflict"};

/*
 * Record abort count
 *
 * This function is called from rcx-hash-list.c whenever RTM aborts.
 */
void record_abort(int stat)
{
	struct rtm_abort_cnt *cnt;
	cnt = &get_cpu_var(abort_cnt);

	cnt->nr_aborts++;
	if (stat & _XABORT_EXPLICIT)
		cnt->counts[ABORT_RTM_EXPLICIT]++;
	if (stat & _XABORT_RETRY)
		cnt->counts[ABORT_RTM_RETRY]++;
	if (stat & _XABORT_CONFLICT)
		cnt->counts[ABORT_RTM_CONFLICT]++;
	if (stat & _XABORT_CAPACITY)
		cnt->counts[ABORT_RTM_CAPACITY]++;
	if (stat & _XABORT_DEBUG)
		cnt->counts[ABORT_RTM_DEBUG]++;
	if (stat & _XABORT_NESTED)
		cnt->counts[ABORT_RTM_NESTED]++;
	if (_XABORT_CODE(stat) == ABORT_DOUBLE_FREE)
		cnt->counts[ABORT_DOUBLE_FREE]++;
	if (_XABORT_CODE(stat) == ABORT_CONFLICT)
		cnt->counts[ABORT_CONFLICT]++;
	if (_XABORT_CODE(stat) == ABORT_LF_CONFLICT)
		cnt->counts[ABORT_LF_CONFLICT]++;

	put_cpu_var(abort_cnt);
}

void pr_abort_stat(struct result_stat *stat)
{
	unsigned long sum[NR_ABORT_REASONS] = {0,};
	unsigned long nr_total_aborts = 0;
	int cpu;
	int i;

	for_each_online_cpu(cpu) {
		struct rtm_abort_cnt *cnt = &per_cpu(abort_cnt, cpu);

		nr_total_aborts += cnt->nr_aborts;
		for (i = 0; i < NR_ABORT_REASONS; i++)
			sum[i] += cnt->counts[i];
	}

	if (stat->duration_ms == 0) stat->duration_ms = 1;
	if (stat->nr_issued_ops == 0) stat->nr_issued_ops = 1;
	if (stat->nr_succ_ops == 0) stat->nr_succ_ops = 1;
	if (stat->nr_upd == 0) stat->nr_upd = 1;

	pr_info("aborts_per_sec: %lu\n",
			nr_total_aborts * 1000 / stat->duration_ms);
	pr_info("aborts_per_1000issued: %lu\n",
			nr_total_aborts * 1000 / stat->nr_issued_ops);
	pr_info("aborts_per_1000succ: %lu\n",
			nr_total_aborts * 1000 / stat->nr_succ_ops);
	pr_info("aborts_per_1000upd: %lu\n",
			nr_total_aborts * 1000 / stat->nr_upd);

	pr_info("nr_total_aborts: %lu\n", nr_total_aborts);
	for (i = 0; i < NR_ABORT_REASONS; i++)
		pr_info("%s: %lu\n", str_abort_reasons[i], sum[i]);
}
