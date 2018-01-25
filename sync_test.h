#ifndef _SYNC_TEST_H
#define _SYNC_TEST_H

#include <linux/completion.h>   /* complete/wait_for_completion */

int benchmark_endtime(void);
extern struct completion sync_test_working;

#endif
