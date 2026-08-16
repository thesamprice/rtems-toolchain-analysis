/* Minimal, fully traced: cancel a thread blocked in read(). Tells us whether
   read returns, and whether the thread actually cancels. write()-only tracing
   (async-signal-safe, unbuffered). */
#define _GNU_SOURCE
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int pfd[2];
static void say(const char *s) { write(2, s, strlen(s)); }

static void *w(void *a)
{
	(void)a;
	say("  worker: entering read()\n");
	char b[8];
	ssize_t n = read(pfd[0], b, 8);
	char m[64];
	int l = snprintf(m, sizeof m, "  worker: read RETURNED n=%zd errno=%d\n",
			 n, errno);
	write(2, m, l);
	return (void *)0x1234;
}

int main(void)
{
	if (pipe(pfd) < 0) return 2;
	pthread_t th;
	pthread_create(&th, NULL, w, NULL);
	sleep(1);
	say("main: calling pthread_cancel\n");
	int rc = pthread_cancel(th);
	char m[64]; int l = snprintf(m, sizeof m, "main: pthread_cancel rc=%d\n", rc);
	write(2, m, l);
	say("main: calling pthread_join\n");
	void *ret;
	pthread_join(th, &ret);
	if (ret == PTHREAD_CANCELED)
		say("main: JOINED, thread was CANCELED  <-- cancel works\n");
	else {
		l = snprintf(m, sizeof m, "main: JOINED, ret=%p (NOT canceled)\n", ret);
		write(2, m, l);
	}
	say("CANCEL-MIN DONE\n");
	return 0;
}
