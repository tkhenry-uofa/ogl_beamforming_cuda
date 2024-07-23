/* See LICENSE for license details. */
#ifndef _UTIL_H_
#define _UTIL_H_

#include <stddef.h>
#include <stdint.h>

#ifdef _DEBUG
	#define ASSERT(c) do { if (!(c)) asm("int3; nop"); } while (0);
	#define DEBUG_EXPORT
#else
	#define ASSERT(c)
	#define DEBUG_EXPORT static
#endif

#define static_assert _Static_assert

#define ARRAY_COUNT(a) (sizeof(a) / sizeof(*a))
#define ABS(x)         ((x) < 0 ? (-x) : (x))
#define CLAMP(x, a, b) ((x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x))
#define ISPOWEROF2(a)  (((a) & ((a) - 1)) == 0)
#define MAX(a, b)      ((a) > (b) ? (a) : (b))
#define ORONE(x)       ((x)? (x) : 1)

#define MEGABYTE (1024ULL * 1024ULL)
#define GIGABYTE (1024ULL * 1024ULL * 1024ULL)

#define U32_MAX  (0xFFFFFFFFUL)

typedef uint8_t   u8;
typedef int16_t   i16;
typedef int32_t   i32;
typedef uint32_t  u32;
typedef uint32_t  b32;
typedef float     f32;
typedef double    f64;
typedef ptrdiff_t size;

typedef struct { u8 *beg, *end; } Arena;

typedef struct { size len; u8 *data; } s8;

typedef union {
	struct { u32 x, y; };
	struct { u32 w, h; };
	u32 E[2];
} uv2;

typedef union {
	struct { u32 x, y, z, w; };
	u32 E[4];
} uv4;

#include "util.c"

#endif /* _UTIL_H_ */
