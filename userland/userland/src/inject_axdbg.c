/* Debug library, injected into backboardd, SpringBoard and Preferences (root26-darwin.sh): do the Reduce
 * Transparency and Reduce Motion settings arrive? */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <notify.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>

static void
say(const char *format, ...)
{
	char line[1100];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		write(fd, line, n);
		close(fd);
	}
}

/* ---- Debug: do the Reduce Transparency / Reduce Motion settings arrive? ---------------------------------
 * Logs, in backboardd, SpringBoard and Settings (Preferences), at start, on every com.apple.accessibility.cache.*
 * notification, and whenever the picture changes (checked every 3 s): libAccessibility's own answers
 * (_AXSEnhanceBackgroundContrastEnabled = Reduce Transparency, _AXSReduceMotionEnabled), the notify state of the
 * cache notifications, and the com.apple.Accessibility keys that mention contrast, transparency or motion.
 * Look for "hakios26: ax[" lines. */
static const char *const ax_cache_names[] = {
	"com.apple.accessibility.cache.enhance.background.contrast",
	"com.apple.accessibility.cache.reduce.motion",
};
static int ax_tokens[2] = { -1, -1 };

static void
ax_report(const char *why)
{
	static char last[1024];
	char line[1024];
	int n;
	unsigned long ret = 0;
	Boolean (*contrast)(void) = (Boolean (*)(void))dlsym(RTLD_DEFAULT, "_AXSEnhanceBackgroundContrastEnabled");
	Boolean (*motion)(void) = (Boolean (*)(void))dlsym(RTLD_DEFAULT, "_AXSReduceMotionEnabled");
	CFStringRef domain = CFSTR("com.apple.Accessibility");

	(void)ret;
	n = snprintf(line, sizeof(line), "ax[%s %s]: contrast()=%d motion()=%d", getprogname(), why,
	    contrast ? (int)contrast() : -1, motion ? (int)motion() : -1);
	for (int i = 0; i < 2; i++) {
		uint64_t state = 0;
		uint32_t r = ax_tokens[i] >= 0 ? notify_get_state(ax_tokens[i], &state) : 99;

		n += snprintf(line + n, sizeof(line) - n, " state%d=%llu(r%u)", i, (unsigned long long)state, r);
	}
	CFPreferencesAppSynchronize(domain);
	CFArrayRef keys = CFPreferencesCopyKeyList(domain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
	n += snprintf(line + n, sizeof(line) - n, " keys=%ld", keys ? (long)CFArrayGetCount(keys) : -1L);
	for (CFIndex i = 0; keys && i < CFArrayGetCount(keys) && n < (int)sizeof(line) - 120; i++) {
		CFStringRef key = CFArrayGetValueAtIndex(keys, i);
		char name[96], value[48] = "?";
		CFTypeRef v;

		if (!CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8) ||
		    (!strcasestr(name, "contrast") && !strcasestr(name, "transpar") && !strcasestr(name, "motion"))) {
			continue;
		}
		v = CFPreferencesCopyAppValue(key, domain);
		if (v != NULL && CFGetTypeID(v) == CFBooleanGetTypeID()) {
			snprintf(value, sizeof(value), "%d", CFBooleanGetValue(v));
		} else if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID()) {
			long long x = 0;
			CFNumberGetValue(v, kCFNumberLongLongType, &x);
			snprintf(value, sizeof(value), "%lld", x);
		}
		if (v != NULL) {
			CFRelease(v);
		}
		n += snprintf(line + n, sizeof(line) - n, " %s=%s", name, value);
	}
	if (keys != NULL) {
		CFRelease(keys);
	}
	if (strcmp(why, "poll") != 0 || strcmp(last, strchr(line, ']')) != 0) {
		say("hakios26: %s\n", line);
	}
	snprintf(last, sizeof(last), "%s", strchr(line, ']'));
}

static void
ax_poll(void *unused)
{
	(void)unused;
	ax_report("poll");
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), NULL, ax_poll);
}

static void
ax_debug_start(void)
{
	for (int i = 0; i < 2; i++) {
		notify_register_check(ax_cache_names[i], &ax_tokens[i]);
		int token;
		notify_register_dispatch(ax_cache_names[i], &token, dispatch_get_global_queue(0, 0), ^(int t) {
			(void)t;
			ax_report("notify");
		});
	}
	ax_report("start");
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), NULL, ax_poll);
}

__attribute__((constructor))
static void
axdbg_init(void)
{
	ax_debug_start();
}
