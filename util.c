/* See LICENSE for license details. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static void __attribute__((noreturn))
die(char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	exit(1);
}

static void *
mem_clear(u8 *p, u8 c, size len)
{
	while (len) p[--len] = c;
	return p;
}

#define alloc(a, t, n)  (t *)alloc_(a, sizeof(t), _Alignof(t), n)
__attribute((malloc, alloc_size(4, 2), alloc_align(3)))
static void *
alloc_(Arena *a, size len, size align, size count)
{
	size padding   = -(uintptr_t)a->beg & (align - 1);
	size available = a->end - a->beg - padding;
	if (available < 0 || count > available / len) {
		ASSERT(0);
		die("arena OOM\n");
	}
	void *p = a->beg + padding;
	a->beg += padding + count * len;
	/* TODO: Performance? */
	return mem_clear(p, 0, count * len);
}

static s8
s8alloc(Arena *a, size len)
{
	return (s8){ .data = alloc(a, u8, len), .len = len };
}
