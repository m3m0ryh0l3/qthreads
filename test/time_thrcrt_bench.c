#ifdef HAVE_CONFIG_H
# include "config.h" /* for _GNU_SOURCE */
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <qthread/qthread.h>
#include <qtimer.h>

#include <pthread.h>

#define NUM_THREADS 1000000
#define MAX_THREADS 400
#define THREAD_BLOCK 200

aligned_t qincr(qthread_t *me, void *arg)
{
    return 0;
}

int main(int argc, char *argv[])
{
    aligned_t rets[MAX_THREADS];
    size_t i;
    int threads = 0;
    int interactive = 0;
    qthread_t *me;
    qtimer_t timer = qtimer_new();
    double cumulative_time = 0.0;
    size_t counter = 0;

    if (argc >= 2) {
	threads = strtol(argv[1], NULL, 0);
	if (threads < 0) {
	    threads = 0;
	    interactive = 0;
	} else {
	    printf("threads: %i\n", threads);
	    interactive = 1;
	}
    }

    if (qthread_init(threads) != QTHREAD_SUCCESS) {
	fprintf(stderr, "qthread library could not be initialized!\n");
	exit(EXIT_FAILURE);
    }
    me = qthread_self();

    for (int iteration = 0; iteration < 10; iteration++) {
	qtimer_start(timer);
	for (int i=0; i<MAX_THREADS; i++) {
	    qthread_fork(qincr, NULL, &(rets[i]));
	}
	counter = MAX_THREADS;
	while (counter < NUM_THREADS) {
	    for (int i=0; i<THREAD_BLOCK; i++) {
		qthread_readFF(me, NULL, &(rets[i]));
	    }
	    for (int i=0; i<THREAD_BLOCK; i++) {
		qthread_fork(qincr, &counter, &(rets[i]));
	    }
	    for (int i=THREAD_BLOCK; i<MAX_THREADS; i++) {
		qthread_readFF(me, NULL, &(rets[i]));
	    }
	    for (int i=THREAD_BLOCK; i<MAX_THREADS; i++) {
		qthread_fork(qincr, &counter, &(rets[i]));
	    }
	    counter += MAX_THREADS;
	}
	for (int i=0; i<MAX_THREADS; i++) {
	    qthread_readFF(me, NULL, &(rets[i]));
	}
	qtimer_stop(timer);
	cumulative_time += qtimer_secs(timer);
    }
    printf("qthread time: %f\n", cumulative_time/10.0);

    qthread_finalize();
    return 0;
}
