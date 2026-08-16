/*
 * pc-probe: determine exactly where the kernel leaves the interrupted PC
 * when a signal (SA_RESTART, like glibc's SIGCANCEL) hits a thread blocked
 * in read(), relative to glibc's __syscall_cancel_arch_{start,end} labels.
 *
 * cancellation_pc_check() fires the cancel only if start <= pc < end.  If
 * the kernel's restartable-syscall PC lands at/after end, read()-based
 * cancellation can never fire (the syscall just restarts) -- the hang we
 * see.  This prints pc, start, end so we can see which side of `end' it is.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <pthread.h>
#include <stdint.h>

extern const char __syscall_cancel_arch_start[];
extern const char __syscall_cancel_arch_end[];

static volatile uintptr_t captured_pc;
static volatile int fired;

static void probe(int sig, siginfo_t *si, void *ctx)
{
	(void)sig; (void)si;
	ucontext_t *u = ctx;
	captured_pc = (uintptr_t)u->uc_mcontext.regs.pc;
	fired = 1;
}

static int pfd[2];
static void *blocker(void *a)
{
	(void)a;
	char b[8];
	read(pfd[0], b, 8);       /* block in the cancellable read bridge */
	return NULL;
}

int main(void)
{
	struct sigaction sa = { .sa_sigaction = probe,
				.sa_flags = SA_SIGINFO | SA_RESTART };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGRTMIN, &sa, NULL);

	if (pipe(pfd) < 0) return 2;

	pthread_t th;
	pthread_create(&th, NULL, blocker, NULL);

	struct timespec t = { 0, 400000000 };
	nanosleep(&t, NULL);              /* let it reach read() */
	pthread_kill(th, SIGRTMIN);       /* interrupt it, SA_RESTART -> restarts */
	nanosleep(&t, NULL);              /* give the handler time to fire */

	uintptr_t start = (uintptr_t)__syscall_cancel_arch_start;
	uintptr_t end   = (uintptr_t)__syscall_cancel_arch_end;
	printf("PROBE fired=%d\n", fired);
	printf("start = 0x%08lx\n", (unsigned long)start);
	printf("end   = 0x%08lx  (range size %ld bytes)\n",
	       (unsigned long)end, (long)(end - start));
	printf("pc    = 0x%08lx\n", (unsigned long)captured_pc);
	if (fired) {
		long off_start = (long)(captured_pc - start);
		long off_end   = (long)(captured_pc - end);
		printf("pc - start = %+ld\n", off_start);
		printf("pc - end   = %+ld  --> %s\n", off_end,
		       (captured_pc >= start && captured_pc < end)
		       ? "IN range (cancel WOULD fire)"
		       : "OUT of range (cancel would NOT fire -> hang)");
	}
	/* unblock so we exit cleanly */
	char x = 0; write(pfd[1], &x, 1);
	pthread_join(th, NULL);
	printf("PROBE END\n");
	return 0;
}
