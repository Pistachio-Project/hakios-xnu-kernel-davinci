/*
 * libhakios-haptic.dylib: UIKit's feedback generators (the switches of Settings, the menus of a long press, the
 * home screen's, the keyboard is not among them) answered on the motor when Darwin's own way does not.
 *
 * Darwin plays the system's feedbacks as waveforms on the "Actuator" audio device (components/audio/audio_plugin.c ->
 * /dev/hakios-haptic -> drivers/board/haptic_ldo.c). The keyboard's reach it; the logs of 200722 and 203131 show
 * that those of the generators below do not: the actuator's I/O runs for tens of seconds and carries silence (the
 * server it plays through is not made: drivers/iokit/BoardHaptics.cpp). So the generators' public calls, impactOccurred
 * (with or without an intensity), selectionChanged and notificationOccurred:, and the click generators' pressedDown and
 * pressedUp, are wrapped: the call goes on as before, and a short waveform of one pulse is written to
 * /dev/hakios-haptic, which the kernel's driver runs the motor for (50 ms at the least). A program that does get its
 * haptics through Darwin's way feels the motor on twice at once, which the driver runs as one vibration.
 *
 * Off with the environment HAKIOS_NO_UIHAPTIC=1. Said on /dev/console: what was wrapped, and each pulse (the first 50).
 */
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PULSE_SAMPLES   640             /* 13 ms at 48 kHz: the driver holds the motor for 50 ms at least */
#define MIN_GAP_MS      40

static void
say(const char *format, ...)
{
	char line[256];
	va_list args;
	int n, fd;

	va_start(args, format);
	n = vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	if (n > 0 && (fd = open("/dev/console", O_WRONLY | O_NOCTTY)) >= 0) {
		(void)write(fd, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
		close(fd);
	}
}

static int motor = -1;
static uint64_t last_pulse;
static unsigned pulses;

static void
pulse(const char *what)
{
	static int16_t wave[PULSE_SAMPLES];
	mach_timebase_info_data_t tb;
	uint64_t now = mach_absolute_time();

	mach_timebase_info(&tb);
	if (last_pulse != 0 && (now - last_pulse) * tb.numer / tb.denom < MIN_GAP_MS * 1000000ull) {
		return;
	}
	last_pulse = now;
	if (motor == -1) {
		motor = open("/dev/hakios-haptic", O_WRONLY | O_NONBLOCK);
		if (wave[0] == 0) {
			for (int i = 0; i < PULSE_SAMPLES; i++) {
				wave[i] = (i / 140) & 1 ? -20000 : 20000;   /* about 170 Hz */
			}
		}
	}
	if (motor >= 0) {
		(void)write(motor, wave, sizeof(wave));
	}
	if (pulses++ < 50) {
		say("hakios: uihaptic: %s pid %d: %s\n", what, getpid(), motor >= 0 ? "pulse written" : "no /dev/hakios-haptic");
	}
}

/* every hooked method: where it was, and the original */
struct hook {
	Class cls;
	SEL sel;
	IMP original;
};
static struct hook hooks[64];
static unsigned hooked;

static __thread Class resume;
static __thread SEL resume_sel;          /* inside a wrapped method that calls its superclass's: where the search goes on */

/* The original of the first wrapped method from `from` up; its class in `*at`. */
static IMP
original_of(Class from, SEL cmd, Class *at)
{
	for (Class c = from; c != NULL; c = class_getSuperclass(c)) {
		for (unsigned i = 0; i < hooked; i++) {
			if (hooks[i].cls == c && hooks[i].sel == cmd) {
				*at = c;
				return hooks[i].original;
			}
		}
	}
	return NULL;
}

/*
 * One function for every selector: on arm64 an integer argument is in x2 and a floating one in d0, never in each
 * other's registers, so a method of either kind (or of none) is called on with what it was called with. A wrapped
 * method that calls up to a wrapped superclass's goes on from the class above (and the pulse is made once).
 */
static void
wrapper(id self, SEL cmd, uintptr_t integer, double real)
{
	Class outer = resume_sel == cmd ? resume : NULL, at = NULL, saved = resume;
	SEL saved_sel = resume_sel;
	IMP original = original_of(outer != NULL ? outer : object_getClass(self), cmd, &at);

	if (outer == NULL) {
		pulse(sel_getName(cmd));
	}
	if (original != NULL) {
		resume = class_getSuperclass(at);
		resume_sel = cmd;
		((void (*)(id, SEL, uintptr_t, double))original)(self, cmd, integer, real);
		resume = saved;
		resume_sel = saved_sel;
	}
}

static const char *const wanted[] = {
	"impactOccurred", "impactOccurredWithIntensity:", "selectionChanged", "notificationOccurred:",
	"pressedDown", "pressedUp",
};

static bool
suffix(const char *s, const char *end)
{
	size_t a = strlen(s), b = strlen(end);

	return a >= b && strcmp(s + a - b, end) == 0;
}

/* The generators' classes, once UIKit has them; true when it has. */
static bool
install(void)
{
	unsigned count = 0, found = 0;
	Class *all;

	if (objc_getClass("UIFeedbackGenerator") == NULL) {
		return false;
	}
	all = objc_copyClassList(&count);
	for (unsigned i = 0; i < count; i++) {
		const char *name = class_getName(all[i]);
		unsigned n = 0;
		Method *methods;

		if (!suffix(name, "FeedbackGenerator")) {
			continue;
		}
		found++;
		methods = class_copyMethodList(all[i], &n);
		for (unsigned m = 0; m < n && hooked < 64; m++) {
			const char *sel = sel_getName(method_getName(methods[m]));

			for (unsigned w = 0; w < sizeof(wanted) / sizeof(wanted[0]); w++) {
				if (strcmp(sel, wanted[w]) == 0) {
					hooks[hooked].cls = all[i];
					hooks[hooked].sel = method_getName(methods[m]);
					hooks[hooked].original = method_setImplementation(methods[m], (IMP)wrapper);
					say("hakios: uihaptic: wrapped -[%s %s]\n", name, sel);
					hooked++;
				}
			}
		}
		free(methods);
	}
	free(all);
	say("hakios: uihaptic: pid %d: %u generator classes, %u methods wrapped\n", getpid(), found, hooked);
	return true;
}

static void
try_install(int attempt)
{
	if (install() || attempt >= 120) {
		return;
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		try_install(attempt + 1);
	});
}

__attribute__((constructor)) static void
start(void)
{
	if (getenv("HAKIOS_NO_UIHAPTIC") != NULL) {
		return;
	}
	try_install(0);
}
