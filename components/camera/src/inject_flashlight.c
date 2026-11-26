/*
 * libhakios-flash.dylib: inside SpringBoard, iOS 27's flashlight control reaching the kernel's flashlight.
 *
 * What stood between a tap on Control Center's Flashlight control and cameracaptured (whose flashlight server,
 * com.apple.coremedia.flashlight, libhakios-capture.dylib gives the kernel's /dev/hakios-torch):
 *
 *   ControlCenterControlsExtension's FlashlightControl is only the control's face (symbol, title, state): the
 *   extension has no flashlight code and no MobileGestalt check. ControlCenterUI maps its kind
 *   (com.apple.ControlCenter.ControlCenterControlsExtension.flashlight) to the module
 *   com.apple.control-center.FlashlightModule (/System/Library/ControlCenter/Bundles/FlashlightModule.bundle,
 *   NSPrincipalClass CCUIFlashlightModule, UIRequiredDeviceCapabilities camera-flash), which runs in SpringBoard
 *   and works SBUIFlashlightController (SpringBoardUI: -turnFlashlightOnForReason:, whose -init makes its AVFlashlight
 *   on a background queue).
 *   -[AVFlashlight init] (AVFCapture) is
 *       if (![AVFlashlight hasFlashlight]) return nil;   // hasFlashlight: MGGetBoolAnswer(@"camera-flash"), once
 *   and only after it does -_setupFlashlightPreservingState: create the FigFlashlightRemote that sends the
 *   message to the flashlight server. Where Darwin's MobileGestalt says there is no flash (the device tree's
 *   /product/camera "flash", which the loader publishes with torch=N; MobileGestalt's own cache, made once, keeps
 *   an answer it gave before that), the controller has no flashlight and the tap is a no-op: no client ever
 *   reaches the server, which is what the run showed.
 *
 * So here, where the kernel has a flashlight (/dev/hakios-torch), the answer is made yes at its two sources in
 * SpringBoard: MobileGestalt's "camera-flash" (dyld interposing of MGGetBoolAnswer and MGCopyAnswer, for that
 * key alone) and +[AVFlashlight hasFlashlight] (found by name, its type checked). Everything else of the
 * flashlight is Apple's. What the controller does is named in the log either way (the answers MobileGestalt
 * and AVFlashlight gave before, the AVFlashlight made or not, the tap), so a run says where it stops.
 *
 * The tap itself (found in the module's code, FlashlightModule.bundle): CCUIFlashlightModuleViewController
 * -buttonTapped:forEvent: asks [SBUIFlashlightController isAvailable] first and calls turnFlashlightOnForReason: only
 * if it is yes; otherwise it calls only turnFlashlightOffForReason:. That is why the tap never reached the "on" hook:
 * isAvailable is made yes here too, and where it was NO (nothing below can serve it) the tap's level is also written
 * straight to /dev/hakios-torch.
 *
 * Where there is no /dev/hakios-torch nothing is changed. A method whose type differs from the one expected is
 * left alone and the log says so.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

extern Boolean MGGetBoolAnswer(CFStringRef key);
extern CFTypeRef MGCopyAnswer(CFStringRef key);

static bool kernel_torch;               /* /dev/hakios-torch is there */
static bool said_mg, said_copy;

static void
say(const char *format, ...) __printflike(1, 2);

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = (int)sizeof(line) - 1;
	}
	if (n > 0) {
		write(2, line, (size_t)n);
	}
}

static bool
is_camera_flash(CFStringRef key)
{
	return key != NULL && CFStringCompare(key, CFSTR("camera-flash"), 0) == kCFCompareEqualTo;
}

/* ---- MobileGestalt's camera-flash ---- */

static Boolean
flash_MGGetBoolAnswer(CFStringRef key)
{
	Boolean was = MGGetBoolAnswer(key);

	if (kernel_torch && is_camera_flash(key)) {
		if (!said_mg) {
			said_mg = true;
			say("hakios: flash: MobileGestalt camera-flash was %s, it is yes (the kernel's flashlight)\n", was ? "yes" : "NO");
		}
		return true;
	}
	return was;
}

static CFTypeRef
flash_MGCopyAnswer(CFStringRef key)
{
	CFTypeRef was = MGCopyAnswer(key);

	if (kernel_torch && is_camera_flash(key)) {
		if (!said_copy) {
			said_copy = true;
			say("hakios: flash: MGCopyAnswer(camera-flash) was %s, it is yes\n",
			    was == kCFBooleanTrue ? "yes" : (was == NULL ? "nothing" : "NO"));
		}
		if (was != NULL) {
			CFRelease(was);
		}
		return CFRetain(kCFBooleanTrue);
	}
	return was;
}

__attribute__((used)) static struct {
	const void *ours, *theirs;
} interposed[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)flash_MGGetBoolAnswer, (const void *)MGGetBoolAnswer },
	{ (const void *)flash_MGCopyAnswer, (const void *)MGCopyAnswer },
};

/* ---- the methods ---- */

/* a method's type encoding without the offsets ("B16@0:8" -> "B@:") */
static bool
types_are(Method m, const char *expected)
{
	const char *t = method_getTypeEncoding(m);
	char out[32];
	size_t n = 0;

	if (t == NULL) {
		return false;
	}
	for (; *t != '\0' && n + 1 < sizeof(out); t++) {
		if (*t < '0' || *t > '9') {
			out[n++] = *t;
		}
	}
	out[n] = '\0';
	return strcmp(out, expected) == 0;
}

static BOOL (*original_has)(id, SEL);
static id (*original_init)(id, SEL);
static void (*original_turn_on)(id, SEL, id);
static bool has_done, init_done, turn_done, off_done, avail_done, tap_done;
static BOOL (*original_avail)(id, SEL);
static void (*original_turn_off)(id, SEL, id);
static void (*original_tap)(id, SEL, id, id);
static int avail_was = -1;              /* what SBUIFlashlightController's own isAvailable said last (1 yes, 0 no) */
static int torch_fd = -2;               /* /dev/hakios-torch, opened at the first direct write */

/* +[AVFlashlight hasFlashlight] */
static BOOL
flash_has(id self, SEL _cmd)
{
	BOOL was = original_has(self, _cmd);

	say("hakios: flash: +[AVFlashlight hasFlashlight] was %s, it is yes\n", was ? "yes" : "NO");
	return YES;
}

/* -[AVFlashlight init]: only named; nil is what it gives where there is no flashlight (or no server) */
static id
flash_init(id self, SEL _cmd)
{
	id made = original_init(self, _cmd);

	say("hakios: flash: -[AVFlashlight init] gives %s\n", made != nil ? "a flashlight (its server answered)" : "NOTHING");
	return made;
}

/* One level (0 to 1000) straight to the kernel's flashlight: where the controller's chain (AVFlashlight, then the
 * flashlight server in cameracaptured) says it is unavailable, the tap would reach nothing. */
static void
torch_direct(uint32_t level)
{
	ssize_t n;

	if (torch_fd == -2) {
		torch_fd = open("/dev/hakios-torch", O_WRONLY);
	}
	n = torch_fd >= 0 ? write(torch_fd, &level, sizeof(level)) : -1;
	say("hakios: flash: written straight to /dev/hakios-torch: level %u%s\n", level,
	    n == (ssize_t)sizeof(level) ? "" : " (NOT TAKEN)");
}

/* -[SBUIFlashlightController isAvailable]: what CCUIFlashlightModuleViewController -buttonTapped:forEvent: asks first
 * (it turns the flashlight on only if this is yes; otherwise the tap only calls turnFlashlightOffForReason:). */
static BOOL
flash_avail(id self, SEL _cmd)
{
	BOOL was = original_avail(self, _cmd);

	if ((int)was != avail_was) {
		avail_was = was;
		say("hakios: flash: -[SBUIFlashlightController isAvailable] was %s, it is yes\n", was ? "yes" : "NO");
	}
	return YES;
}

/* -[CCUIFlashlightModuleViewController buttonTapped:forEvent:]: the tap on Control Center's flashlight toggle */
static void
flash_tap(id self, SEL _cmd, id button, id event)
{
	say("hakios: flash: TAP on the Control Center flashlight toggle (controller said available: %s)\n",
	    avail_was < 0 ? "not asked yet" : (avail_was ? "yes" : "NO"));
	original_tap(self, _cmd, button, event);
}

/* -[SBUIFlashlightController turnFlashlightOffForReason:] */
static void
flash_turn_off(id self, SEL _cmd, id reason)
{
	say("hakios: flash: the flashlight is asked for off\n");
	original_turn_off(self, _cmd, reason);
	if (avail_was == 0) {
		torch_direct(0);
	}
}

/* -[SBUIFlashlightController turnFlashlightOnForReason:] */
static void
flash_turn_on(id self, SEL _cmd, id reason)
{
	char text[64] = "?";
	SEL available = sel_registerName("isAvailable");

	if (reason != nil && CFGetTypeID((CFTypeRef)reason) == CFStringGetTypeID()) {
		CFStringGetCString((CFStringRef)reason, text, sizeof(text), kCFStringEncodingUTF8);
	}
	say("hakios: flash: the flashlight is asked for on (%s), the controller says it is %savailable\n", text,
	    class_respondsToSelector(object_getClass(self), available) && ((BOOL (*)(id, SEL))objc_msgSend)(self, available) ? "" : "NOT ");
	original_turn_on(self, _cmd, reason);
	if (avail_was == 0) {
		torch_direct(1000);     /* (the chain below was not available before: the kernel's light is lit from here) */
	}
}

static void
swizzle(Class cls, bool meta, const char *name, const char *types, IMP replacement, IMP *original, bool *done)
{
	Class where = meta ? object_getClass((id)cls) : cls;
	Method m = class_getInstanceMethod(where, sel_registerName(name));

	*done = true;
	if (m == NULL) {
		say("hakios: flash: %c[%s %s] not found: left alone\n", meta ? '+' : '-', class_getName(cls), name);
		return;
	}
	if (!types_are(m, types)) {
		say("hakios: flash: %c[%s %s] has type %s, not %s: left alone\n", meta ? '+' : '-', class_getName(cls), name,
		    method_getTypeEncoding(m), types);
		return;
	}
	*original = method_setImplementation(m, replacement);
	say("hakios: flash: %c[%s %s] hooked\n", meta ? '+' : '-', class_getName(cls), name);
}

/* the classes come with their images: tried now and after each image that is added, until all are found */
static void
try_hooks(void)
{
	Class av = objc_getClass("AVFlashlight");
	Class controller = objc_getClass("SBUIFlashlightController");
	Class module = objc_getClass("CCUIFlashlightModuleViewController");

	if (av != Nil && !has_done) {
		swizzle(av, true, "hasFlashlight", "B@:", (IMP)flash_has, (IMP *)&original_has, &has_done);
	}
	if (av != Nil && !init_done) {
		swizzle(av, false, "init", "@@:", (IMP)flash_init, (IMP *)&original_init, &init_done);
	}
	if (controller != Nil && !avail_done) {
		swizzle(controller, false, "isAvailable", "B@:", (IMP)flash_avail, (IMP *)&original_avail, &avail_done);
	}
	if (controller != Nil && !off_done) {
		swizzle(controller, false, "turnFlashlightOffForReason:", "v@:@", (IMP)flash_turn_off, (IMP *)&original_turn_off,
		    &off_done);
	}
	if (module != Nil && !tap_done) {
		swizzle(module, false, "buttonTapped:forEvent:", "v@:@@", (IMP)flash_tap, (IMP *)&original_tap, &tap_done);
	}
	if (controller != Nil && !turn_done) {
		swizzle(controller, false, "turnFlashlightOnForReason:", "v@:@", (IMP)flash_turn_on, (IMP *)&original_turn_on,
		    &turn_done);
	}
}

static void
image_added(const struct mach_header *header, intptr_t slide)
{
	(void)header;
	(void)slide;
	if (kernel_torch && !(has_done && init_done && turn_done && off_done && avail_done && tap_done)) {
		try_hooks();
	}
}

__attribute__((constructor))
static void
hakios_flash_init(void)
{
	const char *name = getprogname();

	kernel_torch = access("/dev/hakios-torch", F_OK) == 0;
	say("hakios: flash: in \"%s\": %s\n", name != NULL ? name : "(null)",
	    kernel_torch ? "the flashlight is the kernel's (/dev/hakios-torch)" : "no /dev/hakios-torch: nothing changed");
	if (kernel_torch) {
		try_hooks();
		_dyld_register_func_for_add_image(image_added);
	}
}
