/*
 *  Symbols xevd references that the solvers do not export.
 *
 *  The pthread family is the whole of it. xevd only reaches for it when the
 *  decoder is created with more than one thread - xevd_create guards the
 *  spawning loop with "if (ctx->tc.max_task_cnt > 1)" - and this filter always
 *  passes threads = 1, so none of these run. They exist to satisfy the linker
 *  and they fail loudly rather than quietly pretending to have started
 *  something: a decoder that silently believed it had workers would hang the
 *  same way davs2 does next door.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

int pthread_create(void *thread, const void *attr, void *(*start)(void *), void *arg)
{
	(void)thread; (void)attr; (void)start; (void)arg;
	fprintf(stderr, "[EVCDec] pthread_create in a side module: the decoder must be opened with threads = 1\n");
	return EAGAIN;
}

int pthread_join(unsigned long thread, void **retval)
{
	(void)thread;
	if (retval) *retval = NULL;
	return ESRCH;
}

int pthread_attr_init(void *attr) { (void)attr; return 0; }
int pthread_attr_destroy(void *attr) { (void)attr; return 0; }
int pthread_attr_setdetachstate(void *attr, int state) { (void)attr; (void)state; return 0; }

int pthread_cond_init(void *cond, const void *attr) { (void)cond; (void)attr; return 0; }
int pthread_cond_destroy(void *cond) { (void)cond; return 0; }
int pthread_cond_signal(void *cond) { (void)cond; return 0; }

int pthread_cond_wait(void *cond, void *mutex)
{
	(void)cond; (void)mutex;
	/* Nothing else runs here, so a wait could only ever be forever. */
	fprintf(stderr, "[EVCDec] pthread_cond_wait in a single-threaded module would never return\n");
	abort();
	return 0;
}

void __assert_fail(const char *expr, const char *file, unsigned int line, const char *func)
{
	fprintf(stderr, "[EVCDec] assertion failed: %s at %s:%u in %s\n",
	        expr ? expr : "?", file ? file : "?", line, func ? func : "?");
	abort();
}
