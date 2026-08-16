/*
 * mb-msrtest v3: high-rate, layout-independent probe for MSR[C] preservation
 * across async signal delivery on MicroBlaze. Uses a GIANT carry-neutral window
 * so almost every signal lands inside it, a fast POSIX interval timer, AND a
 * spammer thread -- to maximize in-window signal hits (the "jack the interrupt
 * rate way up" trick). Handler forces MSR[C]=1; if the frame round-trips MSR the
 * resumed carry stays 0, else it leaks to 1.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#define MSR_C 0x4u

static volatile int window_active;
static volatile int sig_in_window;
static volatile unsigned long sig_count;
static volatile int stop;

static void handler(int s)
{
	(void)s;
	sig_count++;
	asm volatile("addik r18, r0, -1\n\t"
		     "add   r18, r18, r18\n\t"   /* MSR[C]=1 */
		     ::: "r18");
	if (window_active)
		sig_in_window = 1;
}

static pthread_t main_tid;
static void *spammer(void *a)
{
	(void)a;
	while (!stop)
		pthread_kill(main_tid, SIGUSR1);
	return NULL;
}

int main(void)
{
	struct sigaction sa = { .sa_handler = handler, .sa_flags = SA_RESTART };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGALRM, &sa, NULL);

	main_tid = pthread_self();
	pthread_t th;
	pthread_create(&th, NULL, spammer, NULL);

	/* fast repeating timer -> SIGALRM */
	timer_t tid;
	struct sigevent sev = { .sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGALRM };
	timer_create(CLOCK_MONOTONIC, &sev, &tid);
	struct itimerspec its = { { 0, 60000 }, { 0, 60000 } }; /* 60us */
	timer_settime(tid, 0, &its, NULL);

	unsigned long hits = 0, leaks = 0, iters, target = 6000UL;
	printf("MSRTEST START (giant window)\n"); fflush(stdout);
	for (iters = 0; iters < target; iters++) {
		unsigned long a, b;
		sig_in_window = 0;
		window_active = 1;
		asm volatile("add r18, r0, r0\n\t"            /* force MSR[C]=0 */
			     "mfs %0, rmsr\n\t"               /* a */
			     ".rept 120000\n\t or r0,r0,r0\n\t .endr\n\t"
			     "mfs %1, rmsr\n\t"               /* b */
			     : "=r"(a), "=r"(b) :: "r18");
		window_active = 0;
		if (sig_in_window) {
			hits++;
			if ((a ^ b) & MSR_C)
				leaks++;
		}
		if ((iters & 0x1FF) == 0) {
			printf("MSRTEST iters=%lu sigs=%lu hits=%lu leaks=%lu\n",
			       iters, sig_count, hits, leaks);
			fflush(stdout);
		}
		if (leaks >= 8) { printf("MSRTEST EARLY leak confirmed\n"); break; }
		if (hits >= 4000) break;
	}
	stop = 1;
	pthread_join(th, NULL);
	printf("MSRTEST DONE iters=%lu sigs=%lu hits=%lu leaks=%lu -> %s\n",
	       iters, sig_count, hits, leaks,
	       leaks ? "MSR[C] NOT PRESERVED across signal (BUG)"
		     : (hits ? "MSR[C] preserved" : "INCONCLUSIVE (no in-window signals)"));
	fflush(stdout);
	return leaks ? 1 : 0;
}
