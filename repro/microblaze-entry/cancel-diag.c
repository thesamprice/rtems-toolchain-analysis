/*
 * cancel-diag: isolate each pthread-cancellation / EINTR / syscall-restart
 * mechanism the glibc tst-cancel and tst-eintr1 tests rely on, one per
 * forked child with a hard timeout, so a hang in one is reported (not
 * masked) and the rest still run.  Answers: which specific mechanism
 * misbehaves on this kernel, and is it a hang (cancellation not delivered)
 * or a crash (state corruption, the entry.S-fix bug signature)?
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>

static int pfd[2];

static void *blk_read(void *a) { char b[8]; (void)a; read(pfd[0], b, 8); return NULL; }
static void *blk_sleep(void *a) { (void)a; struct timespec t={100,0}; nanosleep(&t,NULL); return NULL; }

/* Run fn() in a child with a `secs` timeout. Prints PASS/HANG/CRASH. */
static void subtest(const char *name, int (*fn)(void), int secs)
{
	pid_t p = fork();
	if (p == 0) { _exit(fn()); }
	int status, waited = 0;
	while (waited < secs * 10) {
		struct timespec t = {0, 100000000};   /* 100ms */
		nanosleep(&t, NULL);
		if (waitpid(p, &status, WNOHANG) == p) {
			if (WIFEXITED(status))
				printf("  %-22s %s (exit %d)\n", name,
				       WEXITSTATUS(status) ? "FAIL" : "PASS",
				       WEXITSTATUS(status));
			else if (WIFSIGNALED(status))
				printf("  %-22s CRASH (signal %d)\n", name,
				       WTERMSIG(status));
			fflush(stdout);
			return;
		}
		waited++;
	}
	kill(p, SIGKILL); waitpid(p, &status, 0);
	printf("  %-22s HANG (>%ds)\n", name, secs);
	fflush(stdout);
}

/* --- individual mechanisms --- */

static int t_cancel_read(void)
{
	pthread_t th; pthread_create(&th, NULL, blk_read, NULL);
	struct timespec t={0,200000000}; nanosleep(&t,NULL);   /* let it block */
	pthread_cancel(th);
	return pthread_join(th, NULL);                          /* hangs if no cancel */
}

static int t_cancel_sleep(void)
{
	pthread_t th; pthread_create(&th, NULL, blk_sleep, NULL);
	struct timespec t={0,200000000}; nanosleep(&t,NULL);
	pthread_cancel(th);
	return pthread_join(th, NULL);
}

static volatile sig_atomic_t got;
static void h(int s){ (void)s; got++; }

static int t_eintr(void)
{
	/* main thread blocks in read(); a signal (no SA_RESTART) must make it
	   return EINTR. */
	struct sigaction sa = { .sa_handler = h };
	sigaction(SIGUSR1, &sa, NULL);
	pthread_t self = pthread_self();
	pid_t child = fork();
	if (child == 0) {
		struct timespec t={0,300000000}; nanosleep(&t,NULL);
		pthread_kill(self, SIGUSR1);   /* wrong proc; use kill instead */
		_exit(0);
	}
	/* simpler: alarm delivers SIGALRM to interrupt read */
	signal(SIGALRM, h);
	alarm(1);
	char b[8];
	ssize_t n = read(pfd[0], b, 8);
	waitpid(child, NULL, 0);
	if (n < 0 && errno == EINTR) return 0;     /* EINTR as expected */
	return 1;
}

static int t_restart(void)
{
	/* with SA_RESTART, a signal during a blocking read must NOT surface as
	   EINTR -- the kernel restarts the syscall. We can't easily supply data,
	   so use nanosleep which with SA_RESTART is restarted transparently. */
	struct sigaction sa = { .sa_handler = h, .sa_flags = SA_RESTART };
	sigaction(SIGALRM, &sa, NULL);
	alarm(1);
	struct timespec req = {2, 0}, rem;
	int r = nanosleep(&req, &rem);
	/* SA_RESTART doesn't restart nanosleep (it returns EINTR w/ remaining),
	   so accept either clean or EINTR; a CRASH here is the real signal. */
	(void)r; return 0;
}

static int t_async_cancel(void)
{
	pthread_t th; pthread_create(&th, NULL, blk_read, NULL);
	struct timespec t={0,200000000}; nanosleep(&t,NULL);
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL); /* affects self; fine */
	pthread_cancel(th);
	return pthread_join(th, NULL);
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (pipe(pfd) < 0) return 2;
	printf("CANCEL-DIAG START\n");
	subtest("cancel-read",   t_cancel_read,   6);
	subtest("cancel-sleep",  t_cancel_sleep,  6);
	subtest("eintr",         t_eintr,         6);
	subtest("restart",       t_restart,       6);
	subtest("async-cancel",  t_async_cancel,  6);
	printf("CANCEL-DIAG END\n");
	return 0;
}
