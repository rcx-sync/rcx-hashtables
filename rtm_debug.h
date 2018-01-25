#ifndef _RTM_DEBUG_H
#define _RTM_DEBUG_H

/* Intel defined abort code */
#define ABORT_RTM_EXPLICIT 	0
#define ABORT_RTM_RETRY		1
#define ABORT_RTM_CONFLICT	2
#define ABORT_RTM_CAPACITY	3
#define ABORT_RTM_DEBUG		4
#define ABORT_RTM_NESTED	5
/* RCX defined abort code */
#define ABORT_DOUBLE_FREE	6
#define ABORT_CONFLICT		7
#define ABORT_LF_CONFLICT	8
#define NR_ABORT_REASONS	9

void record_abort(int stat);

struct result_stat {
	unsigned long duration_ms;
	unsigned long nr_issued_ops;
	unsigned long nr_succ_ops;
	unsigned long nr_upd;
};

void pr_abort_stat(struct result_stat *stat);

#endif
