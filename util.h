/* See LICENSE for license details. */
#ifndef _UTIL_H_
#define _UTIL_H_

#include <stddef.h>
#include <stdint.h>

#include <immintrin.h>

#ifndef asm
#define asm __asm__
#endif

#ifndef unreachable
#ifdef _MSC_VER
	#define unreachable() __assume(0)
#else
	#define unreachable() __builtin_unreachable()
#endif
#endif

#ifdef _DEBUG
	#ifdef _WIN32
		#define DEBUG_EXPORT __declspec(dllexport)
	#else
		#define DEBUG_EXPORT
	#endif
	#define ASSERT(c) do { if (!(c)) asm("int3; nop"); } while (0);
#else
	#define DEBUG_EXPORT static
	#define ASSERT(c)
#endif

#define static_assert _Static_assert

#define ARRAY_COUNT(a) (sizeof(a) / sizeof(*a))
#define ABS(x)         ((x) < 0 ? (-x) : (x))
#define CLAMP(x, a, b) ((x) < (a) ? (a) : (x) > (b) ? (b) : (x))
#define CLAMP01(x)     CLAMP(x, 0, 1)
#define ISPOWEROF2(a)  (((a) & ((a) - 1)) == 0)
#define MIN(a, b)      ((a) < (b) ? (a) : (b))
#define MAX(a, b)      ((a) > (b) ? (a) : (b))
#define ORONE(x)       ((x)? (x) : 1)

#define MEGABYTE (1024ULL * 1024ULL)
#define GIGABYTE (1024ULL * 1024ULL * 1024ULL)

#define U32_MAX  (0xFFFFFFFFUL)

typedef char      c8;
typedef uint8_t   u8;
typedef int16_t   i16;
typedef uint16_t  u16;
typedef int32_t   i32;
typedef uint32_t  u32;
typedef int64_t   i64;
typedef uint64_t  u64;
typedef uint32_t  b32;
typedef float     f32;
typedef double    f64;
typedef ptrdiff_t size;
typedef ptrdiff_t iptr;

typedef struct { u8 *beg, *end; } Arena;

typedef struct { size len; u8 *data; } s8;
#define s8(s) (s8){.len = ARRAY_COUNT(s) - 1, .data = (u8 *)s}

/* NOTE: raylib stubs */
#ifndef RAYLIB_H
typedef struct { f32 x, y; } Vector2;
typedef struct { f32 x, y, w, h; } Rectangle;
#endif

typedef union {
	struct { i32 x, y; };
	struct { i32 w, h; };
	i32 E[2];
} iv2;

typedef union {
	struct { u32 x, y; };
	struct { u32 w, h; };
	u32 E[2];
} uv2;

typedef union {
	struct { u32 x, y, z, w; };
	u32 E[4];
} uv4;

typedef union {
	struct { f32 x, y; };
	struct { f32 w, h; };
	Vector2 rl;
	f32 E[2];
} v2;

typedef union {
	struct { f32 x, y, z; };
	struct { f32 w, h, d; };
	f32 E[3];
} v3;

typedef union {
	struct { f32 x, y, z, w; };
	struct { f32 r, g, b, a; };
	struct { v3 xyz; f32 _1; };
	struct { f32 _2; v3 yzw; };
	struct { v2 xy, zw; };
	f32 E[4];
} v4;

typedef union {
	struct { v3 x, y, z; };
	v3  c[3];
	f32 E[9];
} m3;

typedef union {
	struct { v2 pos, size; };
	Rectangle rl;
} Rect;

typedef struct {
	iptr  file;
	char *name;
} Pipe;
#define INVALID_FILE (-1)

typedef struct {
	size filesize;
	u64  timestamp;
} FileStats;
#define ERROR_FILE_STATS (FileStats){.filesize = -1}

typedef struct {
	size  widx;
	u8   *data;
	size  cap;
	b32   errors;
} Stream;

#include "util.c"

#endif /* _UTIL_H_ */
