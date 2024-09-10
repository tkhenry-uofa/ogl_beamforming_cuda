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

static void
mem_move(char *src, char *dest, size n)
{
	if (dest < src) while (n) { *dest++ = *src++; n--; }
	else            while (n) { n--; dest[n] = src[n]; }
}

#define alloc(a, t, n)  (t *)alloc_(a, sizeof(t), _Alignof(t), n)
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

static b32
uv4_equal(uv4 a, uv4 b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

static u32
round_down_power_of_2(u32 a)
{
	u32 result = 0x80000000UL >> _lzcnt_u32(a);
	return result;
}

static void
fill_hadamard(i32 *m, u32 dim)
{
	ASSERT(dim && ISPOWEROF2(dim));

	#define IND(i, j) ((i) * dim + (j))
	m[0] = 1;
	for (u32 k = 1; k < dim; k *= 2) {
		for (u32 i = 0; i < k; i++) {
			for (u32 j = 0; j < k; j++) {
				i32 val = m[IND(i, j)];
				m[IND(i + k, j)]     =  val;
				m[IND(i, j + k)]     =  val;
				m[IND(i + k, j + k)] = -val;
			}
		}
	}
	#undef IND
}
