/*
 * mb-r19test: layout-independent probe for "does a syscall preserve the
 * callee-saved register r19 under high interrupt pressure?" on MicroBlaze.
 *
 * Motivation: in the pthread-cancel hang, glibc's sigcancel_handler loads
 * si_pid into r19, calls getpid(), then compares -- and r19 comes back
 * corrupted, so si_pid != getpid() and the cancel is dropped. This isolates
 * exactly that: put a sentinel in r19, do a syscall (brki), read r19 back.
 * A spammer thread + fast timer jack the interrupt rate up so nested
 * interrupts land inside the syscall window.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>

static volatile int stop;
static volatile unsigned long sig_count;
static pthread_t main_tid;

static void h(int s) { (void)s; sig_count++; }
static void *spam(void *a){ (void)a; while(!stop) pthread_kill(main_tid, SIGUSR1); return 0; }

int main(void)
{
	struct sigaction sa = { .sa_handler = h, .sa_flags = SA_RESTART };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGALRM, &sa, NULL);

	main_tid = pthread_self();
	pthread_t th; pthread_create(&th, NULL, spam, NULL);

	timer_t tid;
	struct sigevent sev = { .sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGALRM };
	timer_create(CLOCK_MONOTONIC, &sev, &tid);
	struct itimerspec its = { { 0, 40000 }, { 0, 40000 } };  /* 40us */
	timer_settime(tid, 0, &its, NULL);

	const unsigned long SENT = 0xA5A5A5A5UL;
	unsigned long bad = 0, iters, target = 8000000UL, last_bad_val = 0;
	printf("R19TEST START\n"); fflush(stdout);
	for (iters = 0; iters < target; iters++) {
		unsigned long after;
		asm volatile(
			"add   r19, %1, r0\n\t"   /* r19 = sentinel */
			"addik r12, r0, 172\n\t"  /* syscall nr (getpid); any is fine */
			"brki  r14, 8\n\t"        /* enter/exit kernel */
			"add   %0, r19, r0\n\t"   /* after = r19 (kernel must preserve) */
			: "=r"(after)
			: "r"(SENT)
			: "r19", "r12", "r14", "r3", "memory");
		if (after != SENT) { bad++; last_bad_val = after; }
		if ((iters & 0xFFFFF) == 0) {
			printf("R19TEST iters=%lu sigs=%lu bad=%lu last=%08lx\n",
			       iters, sig_count, bad, last_bad_val);
			fflush(stdout);
		}
		if (bad >= 5) { printf("R19TEST EARLY: r19 corruption confirmed\n"); break; }
	}
	stop = 1; pthread_join(th, NULL);
	printf("R19TEST DONE iters=%lu sigs=%lu bad=%lu last=%08lx -> %s\n",
	       iters, sig_count, bad, last_bad_val,
	       bad ? "r19 NOT preserved across syscall (BUG)" : "r19 preserved");
	fflush(stdout);
	return bad ? 1 : 0;
}
