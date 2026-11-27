/*
 * chronod held until backboardd's window server answers other processes' display queries (iOS 27; etc/inject.conf
 * names chronod).
 *
 * chronod's UIKit asks the window server for the displays (+[CADisplay displays], QuartzCore's query_displays())
 * as it starts. CARenderServer refuses other processes while backboardd's StartWindowServer has not reached its
 * last step, -[CAWindowServer enableOutOfProcessDisplayObservation]; query_displays() retries for five seconds,
 * logs "Unable to query displays from server" and abort()s. On a fresh /var backboardd is slow (the shader cache is
 * empty) and chronod, started on demand, comes inside that window.
 *
 * backboardd's library (userland/userland/src/inject.c, window_server_opens_displays) sets the state of the Darwin
 * notification below to 1 and posts it when that method returns. Here, before main runs, wait for that: the state
 * is checked (notifyd keeps it, so a late chronod does not wait at all), else polled every 100 ms for up to 120 s.
 * notifyd's lookup is the service every process has and the sandbox of daemons allows; if it were refused,
 * notify_register_check fails and chronod goes on at once, as it did before.
 */
#include <fcntl.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DISPLAYS_OPEN_NOTIFICATION "com.hakios.windowserver.displays-open"
#define WAIT_SECONDS 120

static void
say(const char *format, ...)
{
	char line[300];
	va_list ap;
	int fd;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		write(fd, line, strlen(line));
		close(fd);
	}
}

static double
now(void)
{
	return (double)clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e9;
}

__attribute__((constructor))
static void
chronod_waits_for_displays(void)
{
	const char *name = getprogname();
	int token;
	uint64_t state = 0;
	double start = now();

	if (name == NULL || strcmp(name, "chronod") != 0) {
		return;
	}
	if (notify_register_check(DISPLAYS_OPEN_NOTIFICATION, &token) != NOTIFY_STATUS_OK) {
		say("chronod: cannot ask notifyd about the window server (%s); not waiting\n", DISPLAYS_OPEN_NOTIFICATION);
		return;
	}
	while (!(notify_get_state(token, &state) == NOTIFY_STATUS_OK && state == 1)) {
		if (now() - start >= WAIT_SECONDS) {
			say("chronod: the window server did not open its displays in %d s; going on\n", WAIT_SECONDS);
			notify_cancel(token);
			return;
		}
		usleep(100 * 1000);
	}
	notify_cancel(token);
	say("chronod: waited %.1f s for the window server to open its displays to other processes (uptime %.1f s)\n",
	    now() - start, now());
}
