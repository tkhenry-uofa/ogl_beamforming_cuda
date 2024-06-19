/* See LICENSE for license details. */
#ifndef _UTIL_H_
#define _UTIL_H_

#include <stdint.h>
#include <stddef.h>

#ifdef _DEBUG
#define ASSERT(c) do { if (!(c)) asm("int3; nop"); } while (0);
#define DEBUG_EXPORT
#else
#define ASSERT(c)
#define DEBUG_EXPORT static
#endif

typedef uint8_t   u8;
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
	struct { u32 x, y, z; };
	struct { u32 w, h, d; };
	u32 E[3];
} uv3;

typedef union {
	struct { f32 x, y; };
	f32 E[2];
	Vector2 rl;
} v2;

typedef union {
	struct { f32 x, y, z, w; };
	struct { f32 r, g, b, a; };
	f32 E[4];
	Vector4 rl;
} v4;

enum compute_shaders {
//	CS_FORCES,
	CS_HADAMARD,
//	CS_HERCULES,
//	CS_MIN_MAX,
	CS_UFORCES,
	CS_LAST
};

enum program_flags {
	RELOAD_SHADERS = 1 << 0,
	DO_COMPUTE     = 1 << 1,
	DO_DECODE      = 1 << 2,
};

typedef struct {
	u32 programs[CS_LAST];

	/* NOTE: need 3 storage buffers: incoming rf, currently decoding rf, decoded.
	 * last buffer will always be for decoded data. other two will swap everytime there
	 * is new data. current operating idx is stored in rf_data_idx (0 or 1) which needs
	 * to be accessed atomically
	 */
	u32 rf_data_ssbos[3];
	_Atomic u32 rf_data_idx;

	u32  hadamard_ssbo;
	uv2  hadamard_dim;

	uv3 rf_data_dim;
	i32 rf_data_dim_id;
	i32 out_data_dim_id;
} ComputeShaderCtx;

typedef struct {
	Shader    shader;
	Texture2D output;
	i32       out_data_dim_id;
} FragmentShaderCtx;

typedef struct {
	uv2 window_size;
	u32 flags;

	Color bg, fg;

	u32 out_data_ssbo;
	uv3 out_data_dim;

	ComputeShaderCtx  csctx;
	FragmentShaderCtx fsctx;
} BeamformerCtx;

#define MEGABYTE (1024ULL * 1024ULL)
#define GIGABYTE (1024ULL * 1024ULL * 1024ULL)

#define ARRAY_COUNT(a) (sizeof(a) / sizeof(*a))
#define ABS(x)         ((x) < 0 ? (-x) : (x))
#define CLAMP(x, a, b) ((x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x))

#include "util.c"
#endif /*_UTIL_H_ */
