/*
 * hk_trace.h: a log line for each call of a class's methods, for finding out what a tap on a control calls.
 *
 * A method is replaced by a block that logs "hakios: trace: tag -[Class selector] args" and calls the original with
 * the same registers: the four integer or pointer arguments after self and _cmd (x2 to x5) and one floating point
 * one (d0), which is what arm64 passes them in, so the original sees what it was given. The result comes back in
 * x0: methods that return a float, a double or a structure, that take a structure, or that take more than four
 * integer arguments or more than one floating point one, are left alone. Objects are named by class only.
 * The caller says where the line goes (hk_trace_say) and has -fblocks.
 */
#ifndef HK_TRACE_H
#define HK_TRACE_H
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdint.h>
#include <string.h>

extern void hk_trace_say(const char *format, ...) __printflike(1, 2);

typedef uintptr_t (*hk_trace_fn)(id, SEL, uintptr_t, uintptr_t, uintptr_t, uintptr_t, double);

static inline bool
hk_trace_wrappable(Method m)
{
	char type[64];
	unsigned n = method_getNumberOfArguments(m), floats = 0, ints = 0;

	method_getReturnType(m, type, sizeof(type));
	if (type[0] == 'd' || type[0] == 'f' || type[0] == 'D' || type[0] == '{' || type[0] == '(' || type[0] == '[') {
		return false;
	}
	for (unsigned i = 2; i < n; i++) {
		method_getArgumentType(m, i, type, sizeof(type));
		if (type[0] == '{' || type[0] == '(' || type[0] == '[') {
			return false;
		}
		if (type[0] == 'd' || type[0] == 'f') {
			floats++;
		} else {
			ints++;
		}
	}
	return floats <= 1 && ints <= 4;
}

/* one method of a class, by name; false when it is not there, not wrappable or already traced */
static inline bool
hk_trace_method(Class cls, bool meta, Method m, const char *tag)
{
	SEL sel = method_getName(m);
	const char *name = sel_getName(sel);
	const char *cname = class_getName(cls);
	char type[64], args[8];
	unsigned n = method_getNumberOfArguments(m);
	__block IMP original = method_getImplementation(m);
	IMP replacement;

	(void)meta;
	if (!hk_trace_wrappable(m) || strncmp(name, "_dealloc", 8) == 0 || strcmp(name, "dealloc") == 0 ||
	    strcmp(name, ".cxx_destruct") == 0 || strcmp(name, "retain") == 0 || strcmp(name, "release") == 0 ||
	    strcmp(name, "autorelease") == 0 || strcmp(name, "retainCount") == 0) {
		return false;
	}
	for (unsigned i = 2; i < n && i - 2 < sizeof(args) - 1; i++) {
		method_getArgumentType(m, i, type, sizeof(type));
		args[i - 2] = type[0];
		args[i - 1] = '\0';
	}
	if (n <= 2) {
		args[0] = '\0';
	}
	{
		const char *types = strdup(args);

		replacement = imp_implementationWithBlock((id)^uintptr_t(id self, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, double f) {
			const uintptr_t v[4] = { a, b, c, d };
			char text[200];
			size_t at = 0;

			text[0] = '\0';
			for (unsigned i = 0; types[i] != '\0' && i < 4 && at + 40 < sizeof(text); i++) {
				if (types[i] == 'd' || types[i] == 'f') {
					at += (size_t)snprintf(text + at, sizeof(text) - at, " %g", f);
				} else if (types[i] == '@' && v[i] != 0) {
					at += (size_t)snprintf(text + at, sizeof(text) - at, " <%s>", object_getClassName((id)v[i]));
				} else {
					at += (size_t)snprintf(text + at, sizeof(text) - at, " %ld", (long)v[i]);
				}
			}
			hk_trace_say("hakios: trace: %s %c[%s %s]%s\n", tag, meta ? '+' : '-', cname, name, text);
			return ((hk_trace_fn)original)(self, sel, a, b, c, d, f);
		});
	}
	method_setImplementation(m, replacement);
	return true;
}

/* every wrappable instance method of a class: how many were traced */
static inline unsigned
hk_trace_class(Class cls, const char *tag)
{
	unsigned count = 0, total = 0;
	Method *list = class_copyMethodList(cls, &total);

	for (unsigned i = 0; i < total; i++) {
		count += hk_trace_method(cls, false, list[i], tag);
	}
	free(list);
	return count;
}
#endif
